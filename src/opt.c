// The optimisation pipeline and the smaller passes: algebraic
// simplification ("combine"), common subexpression elimination over the
// dominator tree, and dead code elimination.
//
// -O0 only cleans up the CFG.  -O1 runs:
//
//   simplify-cfg, mem2reg,
//   2 x (sccp, combine, cse, load-forward, dce, simplify-cfg),
//   licm, combine, cse, dce
//
// and then splits critical edges for the instruction selector.

#include "ouro.h"

//
// Combine: local rewrites that make later passes and instruction
// selection more effective.  Each rewrite replaces an instruction by an
// equivalent, simpler one.
//

static bool is_const(Inst *i, int64_t v) {
  return i->op == IR_ICONST && i->ival == v;
}

static bool is_commutative(int op) {
  switch (op) {
  case IR_ADD:
  case IR_MUL:
  case IR_AND:
  case IR_OR:
  case IR_XOR:
  case IR_EQ:
  case IR_NE:
  case IR_FADD:
  case IR_FMUL:
    return true;
  default:
    return false;
  }
}

static Func *cf;

static Inst *mk(int op, int ty, Inst *a, Inst *b, Inst *before) {
  Inst *n = ir_new(cf, op, ty, b ? 2 : 1);
  n->args[0] = a;
  if (b)
    n->args[1] = b;
  ir_insert_before(before, n);
  return n;
}

// Whether a canonical I32 value is already sign- (zero-) extended from
// `bits` bits, so that another extension is a no-op.
static bool known_sext(Inst *v, int bits) {
  switch (v->op) {
  case IR_SEXT8:
    return true;
  case IR_SEXT16:
    return bits >= 16;
  case IR_LOAD:
    return v->is_signed && (v->memty == IR_I8 || (v->memty == IR_I16 && bits >= 16));
  case IR_ICONST:
    return bits == 8 ? v->ival == (int8_t)v->ival : v->ival == (int16_t)v->ival;
  default:
    return false;
  }
}

static bool known_zext(Inst *v, int bits) {
  switch (v->op) {
  case IR_ZEXT8:
    return true;
  case IR_ZEXT16:
    return bits >= 16;
  case IR_LOAD:
    return !v->is_signed && (v->memty == IR_I8 || (v->memty == IR_I16 && bits >= 16));
  case IR_ICONST:
    return bits == 8 ? (v->ival & ~0xFFLL) == 0 : (v->ival & ~0xFFFFLL) == 0;
  case IR_AND:
    return v->args[1]->op == IR_ICONST && v->args[1]->ival >= 0 &&
           v->args[1]->ival <= (bits == 8 ? 0xFF : 0xFFFF);
  default:
    return (v->op >= IR_EQ && v->op <= IR_FGE); // comparisons yield 0 or 1
  }
}

static int inverse_compare(int op) {
  switch (op) {
  case IR_EQ:
    return IR_NE;
  case IR_NE:
    return IR_EQ;
  case IR_SLT:
    return IR_SGE;
  case IR_SGE:
    return IR_SLT;
  case IR_SLE:
    return IR_SGT;
  case IR_SGT:
    return IR_SLE;
  case IR_ULT:
    return IR_UGE;
  case IR_UGE:
    return IR_ULT;
  case IR_ULE:
    return IR_UGT;
  case IR_UGT:
    return IR_ULE;
  default:
    return -1;
  }
}

// Returns a replacement for `i`, or NULL.
static Inst *simplify(Inst *i) {
  if (i->nargs == 2 && is_commutative(i->op) && i->args[0]->op == IR_ICONST &&
      i->args[1]->op != IR_ICONST) {
    Inst *t = i->args[0];
    i->args[0] = i->args[1];
    i->args[1] = t;
  }
  Inst *a = i->nargs > 0 ? i->args[0] : NULL;
  Inst *b = i->nargs > 1 ? i->args[1] : NULL;
  int ty = i->ty;
  int bits = ty == IR_I64 ? 64 : 32;
  switch (i->op) {
  case IR_ADD:
    if (is_const(b, 0))
      return a;
    // (x + c1) + c2 -> x + (c1 + c2): longer address chains fold away
    if (b->op == IR_ICONST && a->op == IR_ADD && a->args[1]->op == IR_ICONST && a->nuses == 1) {
      bool ok;
      int64_t c = fold_int(IR_ADD, ty, a->args[1]->ival, b->ival, &ok);
      return mk(IR_ADD, ty, a->args[0], ir_iconst(cf, ty, c), i);
    }
    return NULL;
  case IR_SUB:
    if (is_const(b, 0))
      return a;
    if (a == b)
      return ir_iconst(cf, ty, 0);
    if (b->op == IR_ICONST && b->ival != INT64_MIN)
      return mk(IR_ADD, ty, a, ir_iconst(cf, ty, -b->ival), i);
    return NULL;
  case IR_MUL:
    if (is_const(b, 1))
      return a;
    if (is_const(b, 0))
      return b;
    if (b->op == IR_ICONST && b->ival > 0 && is_pow2((uint64_t)b->ival) &&
        (ty == IR_I64 || b->ival <= 0x40000000))
      return mk(IR_SHL, ty, a, ir_iconst(cf, ty, log2_u64((uint64_t)b->ival)), i);
    return NULL;
  case IR_UDIV:
    if (is_const(b, 1))
      return a;
    if (b->op == IR_ICONST && b->ival > 0 && is_pow2((uint64_t)b->ival))
      return mk(IR_LSHR, ty, a, ir_iconst(cf, ty, log2_u64((uint64_t)b->ival)), i);
    return NULL;
  case IR_UREM:
    if (b->op == IR_ICONST && b->ival > 0 && is_pow2((uint64_t)b->ival))
      return mk(IR_AND, ty, a, ir_iconst(cf, ty, b->ival - 1), i);
    return NULL;
  case IR_SDIV:
    if (is_const(b, 1))
      return a;
    return NULL;
  case IR_AND:
    if (is_const(b, -1))
      return a;
    if (is_const(b, 0))
      return b;
    if (a == b)
      return a;
    if (b->op == IR_ICONST && ty == IR_I32 && b->ival == 0xFF && known_zext(a, 8))
      return a;
    if (b->op == IR_ICONST && ty == IR_I32 && b->ival == 0xFFFF && known_zext(a, 16))
      return a;
    return NULL;
  case IR_OR:
  case IR_XOR:
    if (is_const(b, 0))
      return a;
    if (a == b && i->op == IR_OR)
      return a;
    if (a == b)
      return ir_iconst(cf, ty, 0);
    return NULL;
  case IR_SHL:
  case IR_LSHR:
  case IR_ASHR:
    if (b->op == IR_ICONST && (b->ival & (bits - 1)) == 0)
      return a;
    return NULL;
  case IR_SEXT8:
    if (known_sext(a, 8))
      return a;
    return NULL;
  case IR_SEXT16:
    if (known_sext(a, 16) || known_zext(a, 8))
      return a;
    return NULL;
  case IR_ZEXT8:
    if (known_zext(a, 8))
      return a;
    return NULL;
  case IR_ZEXT16:
    if (known_zext(a, 16))
      return a;
    return NULL;
  case IR_TRUNC:
    if (a->op == IR_SEXT32 || a->op == IR_ZEXT32)
      return a->args[0];
    return NULL;
  case IR_NE:
    // (cmp) != 0 is the comparison itself
    if (is_const(b, 0) && a->ty == IR_I32 && a->op >= IR_EQ && a->op <= IR_FGE)
      return a;
    return NULL;
  case IR_EQ:
    // (cmp) == 0 is the inverse integer comparison
    if (is_const(b, 0) && a->ty == IR_I32 && a->op >= IR_EQ && a->op <= IR_UGE &&
        inverse_compare(a->op) >= 0 && a->nuses == 1)
      return mk(inverse_compare(a->op), IR_I32, a->args[0], a->args[1], i);
    return NULL;
  case IR_COPY:
    return a;
  default:
    return NULL;
  }
}

bool opt_combine(Func *f) {
  cf = f;
  bool any = false;
  for (int round = 0; round < 4; round++) {
    ir_compute_uses(f);
    bool changed = false;
    for (Block *b = f->entry; b; b = b->next)
      for (Inst *i = b->first; i;) {
        Inst *next = i->next;
        for (int k = 0; k < i->nargs; k++)
          i->args[k] = ir_resolve(i->args[k]);
        Inst *r = i->ty != IR_VOID ? simplify(i) : NULL;
        if (r && r != i) {
          ir_unlink(i);
          ir_replace(i, r);
          changed = true;
        }
        i = next;
      }
    ir_resolve_all(f);
    any |= changed;
    if (!changed)
      break;
  }
  ir_verify(f, "combine");
  return any;
}

//
// Common subexpression elimination: a preorder walk of the dominator
// tree with a scoped table of the pure instructions seen on the path
// from the entry.  An instruction equal to one in the table is replaced
// by it, which is valid because that one dominates it.
//

typedef struct {
  Inst **slots;
  int cap;
  int *undo;   // stack of inserted slot indices
  int nundo;
  int undo_cap;
} CseTable;

static uint64_t operand_hash(Inst *a) {
  switch (a->op) {
  case IR_ICONST:
    return (uint64_t)a->ival * 0x9E3779B97F4A7C15ULL + 1;
  case IR_FCONST: {
    uint64_t bits;
    memcpy(&bits, &a->fval, 8);
    return bits * 0x9E3779B97F4A7C15ULL + 2;
  }
  case IR_GADDR:
    return (uint64_t)a->ival * 31 + (uint64_t)a->sym->asm_name[1] * 7 + 3;
  default:
    return (uint64_t)a->id * 0x9E3779B97F4A7C15ULL;
  }
}

static bool operand_eq(Inst *a, Inst *b) {
  if (a == b)
    return true;
  if (a->op != b->op || a->ty != b->ty)
    return false;
  switch (a->op) {
  case IR_ICONST:
    return a->ival == b->ival;
  case IR_FCONST:
    return memcmp(&a->fval, &b->fval, 8) == 0;
  case IR_GADDR:
    return a->sym == b->sym && a->ival == b->ival;
  default:
    return false;
  }
}

static uint64_t inst_hash(Inst *i) {
  uint64_t h = (uint64_t)i->op * 1000003 + (uint64_t)i->ty;
  for (int k = 0; k < i->nargs; k++)
    h = h * 31 + operand_hash(i->args[k]);
  return h;
}

static bool inst_eq(Inst *a, Inst *b) {
  if (a->op != b->op || a->ty != b->ty || a->nargs != b->nargs)
    return false;
  for (int k = 0; k < a->nargs; k++)
    if (!operand_eq(a->args[k], b->args[k]))
      return false;
  return true;
}

static bool cse_candidate(Inst *i) {
  return ir_is_pure(i) && !ir_is_floating(i) && i->ty != IR_VOID && i->nargs > 0;
}

bool opt_cse(Func *f) {
  opt_dominators(f);
  int count = 0;
  for (Block *b = f->entry; b; b = b->next)
    for (Inst *i = b->first; i; i = i->next)
      count++;
  CseTable t;
  t.cap = 64;
  while (t.cap < count * 2)
    t.cap *= 2;
  t.slots = xalloc(sizeof(Inst *) * t.cap);
  t.undo_cap = count + 1;
  t.undo = xalloc(sizeof(int) * t.undo_cap);
  t.nundo = 0;
  bool changed = false;

  typedef struct {
    Block *b;
    int next_kid;
    int undo_mark;
  } Frame;
  Frame *stack = xalloc(sizeof(Frame) * (f->nblocks + 1));
  int sp = 0;
  stack[sp].b = f->entry;
  stack[sp].next_kid = -1;
  sp++;
  while (sp) {
    Frame *fr = &stack[sp - 1];
    if (fr->next_kid < 0) {
      fr->undo_mark = t.nundo;
      fr->next_kid = 0;
      for (Inst *i = fr->b->first; i;) {
        Inst *next = i->next;
        for (int k = 0; k < i->nargs; k++)
          i->args[k] = ir_resolve(i->args[k]);
        if (cse_candidate(i)) {
          uint32_t h = (uint32_t)inst_hash(i) & (t.cap - 1);
          Inst *found = NULL;
          while (t.slots[h]) {
            if (inst_eq(t.slots[h], i)) {
              found = t.slots[h];
              break;
            }
            h = (h + 1) & (t.cap - 1);
          }
          if (found) {
            ir_unlink(i);
            ir_replace(i, found);
            changed = true;
          } else {
            t.slots[h] = i;
            t.undo[t.nundo++] = (int)h;
          }
        }
        i = next;
      }
    }
    if (fr->next_kid < fr->b->nkids) {
      Block *kid = fr->b->kids[fr->next_kid++];
      stack[sp].b = kid;
      stack[sp].next_kid = -1;
      sp++;
      continue;
    }
    // Leaving the subtree: forget what was added in it.  Entries are
    // removed in reverse order, so linear probing stays consistent.
    while (t.nundo > fr->undo_mark)
      t.slots[t.undo[--t.nundo]] = NULL;
    sp--;
  }
  ir_resolve_all(f);
  ir_verify(f, "cse");
  return changed;
}

//
// Dead code elimination: everything not needed by an instruction with
// side effects (stores, calls, control flow) goes.
//

bool opt_dce(Func *f) {
  Vec work = {0};
  for (Block *b = f->entry; b; b = b->next)
    for (Inst *i = b->first; i; i = i->next) {
      i->mark = ir_has_side_effects(i);
      if (i->mark)
        vec_push(&work, i);
    }
  while (work.len) {
    Inst *i = work.data[--work.len];
    for (int k = 0; k < i->nargs; k++) {
      Inst *a = i->args[k];
      if (a->bb && !a->mark) {
        a->mark = true;
        vec_push(&work, a);
      }
    }
  }
  bool changed = false;
  for (Block *b = f->entry; b; b = b->next)
    for (Inst *i = b->first; i;) {
      Inst *next = i->next;
      if (!i->mark) {
        ir_unlink(i);
        i->op = IR_NOP;
        changed = true;
      }
      i = next;
    }
  for (Block *b = f->entry; b; b = b->next)
    for (Inst *i = b->first; i; i = i->next)
      i->mark = false;
  ir_verify(f, "dce");
  return changed;
}

void optimize(Func *f) {
  opt_simplify_cfg(f);
  if (opt_level >= 1) {
    opt_mem2reg(f);
    opt_remove_trivial_phis(f);
    for (int round = 0; round < 2; round++) {
      opt_sccp(f);
      opt_simplify_cfg(f);
      opt_combine(f);
      opt_cse(f);
      opt_load_forward(f);
      opt_dce(f);
      opt_simplify_cfg(f);
    }
    if (opt_licm(f)) {
      opt_combine(f);
      opt_cse(f);
      opt_dce(f);
    }
  }
  opt_split_critical_edges(f);
  opt_loop_depth(f);
}
