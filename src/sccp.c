// Sparse conditional constant propagation (Wegman and Zadeck, "Constant
// Propagation with Conditional Branches", 1991).
//
// Every value starts at TOP (no information yet) and only moves down the
// lattice TOP -> constant -> BOTTOM.  Blocks are visited only once an
// edge into them is known to be executable, so a branch on a constant
// keeps the other side from polluting the phis it feeds.  Afterwards
// constant values replace their instructions and branches whose
// condition is known become jumps; CFG cleanup removes what died.
//
// Folding follows the target exactly (32-bit operations wrap at 32 bits,
// shift amounts are taken modulo the width as arm64 does), and anything
// the C standard leaves undefined, such as division by zero, is left to
// run as written.

#include "ouro.h"

enum { LAT_TOP, LAT_CONST, LAT_BOTTOM };

static Vec flow_work;   // Block*
static Vec ssa_work;    // Inst*
static bool *edge_exec; // per block: executable flags of incoming edges, indexed by pred
static int *edge_base;

static bool block_exec(Block *b) {
  return b->mark;
}

static int lat(Inst *i) {
  if (i->op == IR_ICONST || i->op == IR_FCONST)
    return LAT_CONST;
  if (!i->bb)
    return LAT_BOTTOM; // symbol addresses
  return i->lattice;
}

static int64_t ival(Inst *i) {
  return i->op == IR_ICONST ? i->ival : i->lat_i;
}

static double fval(Inst *i) {
  return i->op == IR_FCONST ? i->fval : i->lat_f;
}

static int64_t norm(int ty, int64_t v) {
  return ty == IR_I32 ? (int64_t)(int32_t)v : v;
}

// Folds an integer operation as arm64 would execute it.  Returns false
// for operations whose C semantics are undefined.
int64_t fold_int(int op, int ty, int64_t a, int64_t b, bool *ok) {
  bool w32 = ty == IR_I32;
  uint64_t ua = w32 ? (uint32_t)a : (uint64_t)a;
  uint64_t ub = w32 ? (uint32_t)b : (uint64_t)b;
  int64_t sa = w32 ? (int32_t)a : a;
  int64_t sb = w32 ? (int32_t)b : b;
  int bits = w32 ? 32 : 64;
  *ok = true;
  switch (op) {
  case IR_ADD:
    return norm(ty, (int64_t)(ua + ub));
  case IR_SUB:
    return norm(ty, (int64_t)(ua - ub));
  case IR_MUL:
    return norm(ty, (int64_t)(ua * ub));
  case IR_SDIV:
  case IR_SREM:
    if (sb == 0 || (sb == -1 && sa == (w32 ? INT32_MIN : INT64_MIN))) {
      *ok = false;
      return 0;
    }
    return norm(ty, op == IR_SDIV ? sa / sb : sa % sb);
  case IR_UDIV:
  case IR_UREM:
    if (ub == 0) {
      *ok = false;
      return 0;
    }
    return norm(ty, (int64_t)(op == IR_UDIV ? ua / ub : ua % ub));
  case IR_AND:
    return norm(ty, a & b);
  case IR_OR:
    return norm(ty, a | b);
  case IR_XOR:
    return norm(ty, a ^ b);
  case IR_SHL:
    return norm(ty, (int64_t)(ua << (ub & (bits - 1))));
  case IR_LSHR:
    return norm(ty, (int64_t)(ua >> (ub & (bits - 1))));
  case IR_ASHR:
    return norm(ty, sa >> (ub & (bits - 1)));
  case IR_EQ:
    return ua == ub;
  case IR_NE:
    return ua != ub;
  case IR_SLT:
    return sa < sb;
  case IR_SLE:
    return sa <= sb;
  case IR_SGT:
    return sa > sb;
  case IR_SGE:
    return sa >= sb;
  case IR_ULT:
    return ua < ub;
  case IR_ULE:
    return ua <= ub;
  case IR_UGT:
    return ua > ub;
  case IR_UGE:
    return ua >= ub;
  default:
    *ok = false;
    return 0;
  }
}

static double round_to(int ty, double v) {
  return ty == IR_F32 ? (double)(float)v : v;
}

// Evaluates a pure instruction whose operands are all constants.
static bool evaluate(Inst *i, int64_t *iv, double *fv) {
  Inst *a = i->nargs > 0 ? i->args[0] : NULL;
  Inst *b = i->nargs > 1 ? i->args[1] : NULL;
  int aty = a ? a->ty : IR_VOID;
  bool ok = true;
  switch (i->op) {
  case IR_COPY:
    if (ir_is_float(i->ty))
      *fv = fval(a);
    else
      *iv = ival(a);
    return true;
  case IR_ADD:
  case IR_SUB:
  case IR_MUL:
  case IR_SDIV:
  case IR_UDIV:
  case IR_SREM:
  case IR_UREM:
  case IR_AND:
  case IR_OR:
  case IR_XOR:
  case IR_SHL:
  case IR_LSHR:
  case IR_ASHR:
    *iv = fold_int(i->op, i->ty, ival(a), ival(b), &ok);
    return ok;
  case IR_EQ:
  case IR_NE:
  case IR_SLT:
  case IR_SLE:
  case IR_SGT:
  case IR_SGE:
  case IR_ULT:
  case IR_ULE:
  case IR_UGT:
  case IR_UGE:
    *iv = fold_int(i->op, aty, ival(a), ival(b), &ok);
    return ok;
  case IR_NEG:
    *iv = norm(i->ty, (int64_t)(0 - (uint64_t)ival(a)));
    return true;
  case IR_NOT:
    *iv = norm(i->ty, ~ival(a));
    return true;
  case IR_FADD:
  case IR_FSUB:
  case IR_FMUL:
  case IR_FDIV: {
    double x = fval(a), y = fval(b);
    if (i->ty == IR_F32) {
      float fx = (float)x, fy = (float)y, r;
      switch (i->op) {
      case IR_FADD:
        r = fx + fy;
        break;
      case IR_FSUB:
        r = fx - fy;
        break;
      case IR_FMUL:
        r = fx * fy;
        break;
      default:
        r = fx / fy;
        break;
      }
      *fv = r;
      return true;
    }
    switch (i->op) {
    case IR_FADD:
      *fv = x + y;
      break;
    case IR_FSUB:
      *fv = x - y;
      break;
    case IR_FMUL:
      *fv = x * y;
      break;
    default:
      *fv = x / y;
      break;
    }
    return true;
  }
  case IR_FNEG:
    *fv = -fval(a);
    return true;
  case IR_FEQ:
    *iv = fval(a) == fval(b);
    return true;
  case IR_FNE:
    *iv = fval(a) != fval(b);
    return true;
  case IR_FLT:
    *iv = fval(a) < fval(b);
    return true;
  case IR_FLE:
    *iv = fval(a) <= fval(b);
    return true;
  case IR_FGT:
    *iv = fval(a) > fval(b);
    return true;
  case IR_FGE:
    *iv = fval(a) >= fval(b);
    return true;
  case IR_SEXT8:
    *iv = (int8_t)ival(a);
    return true;
  case IR_SEXT16:
    *iv = (int16_t)ival(a);
    return true;
  case IR_ZEXT8:
    *iv = (uint8_t)ival(a);
    return true;
  case IR_ZEXT16:
    *iv = (uint16_t)ival(a);
    return true;
  case IR_SEXT32:
    *iv = (int32_t)ival(a);
    return true;
  case IR_ZEXT32:
    *iv = (uint32_t)ival(a);
    return true;
  case IR_TRUNC:
    *iv = (int32_t)ival(a);
    return true;
  case IR_SITOF: {
    int64_t v = aty == IR_I32 ? (int32_t)ival(a) : ival(a);
    *fv = i->ty == IR_F32 ? (double)(float)v : (double)v;
    return true;
  }
  case IR_UITOF: {
    uint64_t v = aty == IR_I32 ? (uint32_t)ival(a) : (uint64_t)ival(a);
    *fv = i->ty == IR_F32 ? (double)(float)v : (double)v;
    return true;
  }
  case IR_FTOSI: {
    double v = fval(a);
    if (i->ty == IR_I32 ? !(v > -2147483649.0 && v < 2147483648.0)
                        : !(v > -9223372036854775809.0 && v < 9223372036854775808.0))
      return false; // out of range: undefined in C, saturating on arm64
    *iv = norm(i->ty, (int64_t)v);
    return true;
  }
  case IR_FTOUI: {
    double v = fval(a);
    if (i->ty == IR_I32 ? !(v > -1.0 && v < 4294967296.0) : !(v > -1.0 && v < 18446744073709551616.0))
      return false;
    *iv = norm(i->ty, (int64_t)(uint64_t)v);
    return true;
  }
  case IR_FEXT:
    *fv = fval(a);
    return true;
  case IR_FTRUNC:
    *fv = round_to(IR_F32, fval(a));
    return true;
  case IR_BITCAST:
    if (ir_is_float(i->ty)) {
      int64_t v = ival(a);
      if (i->ty == IR_F32) {
        int32_t w = (int32_t)v;
        float f;
        memcpy(&f, &w, 4);
        *fv = f;
      } else {
        memcpy(fv, &v, 8);
      }
    } else {
      double d = fval(a);
      if (i->ty == IR_I32) {
        float f = (float)d;
        int32_t w;
        memcpy(&w, &f, 4);
        *iv = w;
      } else {
        memcpy(iv, &d, 8);
      }
    }
    return true;
  default:
    return false;
  }
}

static void set_lattice(Inst *i, int l, int64_t iv, double fv) {
  if (i->lattice == l) {
    if (l != LAT_CONST)
      return;
    if (ir_is_float(i->ty) ? memcmp(&i->lat_f, &fv, sizeof(double)) == 0 : i->lat_i == iv)
      return;
    l = LAT_BOTTOM; // a different constant: cannot happen for monotone input, be safe
  }
  if (i->lattice == LAT_BOTTOM)
    return;
  i->lattice = l;
  i->lat_i = iv;
  i->lat_f = fv;
  vec_push(&ssa_work, i);
}

static void visit(Inst *i);

static void mark_edge(Block *from, Block *to) {
  int idx = ir_pred_index(to, from);
  ASSERT(idx >= 0);
  bool *flag = &edge_exec[edge_base[to->id] + idx];
  if (*flag)
    return;
  *flag = true;
  if (!to->mark) {
    to->mark = true;
    vec_push(&flow_work, to);
  } else {
    // A new executable edge into a reached block: re-evaluate its phis.
    // Their users are notified only if a value actually changes.
    for (Inst *p = to->first; p && p->op == IR_PHI; p = p->next)
      visit(p);
  }
}

static bool same_const(Inst *a, Inst *b, bool is_float) {
  if (is_float) {
    double x = fval(a), y = fval(b);
    return memcmp(&x, &y, sizeof(double)) == 0;
  }
  return ival(a) == ival(b);
}

static void visit(Inst *i) {
  Block *b = i->bb;
  if (!b || !block_exec(b))
    return;
  switch (i->op) {
  case IR_PHI: {
    Inst *c = NULL;
    bool bottom = false;
    for (int k = 0; k < i->nargs; k++) {
      int idx = i->arg_edge[k];
      if (idx < 0 || !edge_exec[edge_base[b->id] + idx])
        continue;
      Inst *a = i->args[k];
      int l = lat(a);
      if (l == LAT_TOP)
        continue;
      if (l == LAT_BOTTOM) {
        bottom = true;
        break;
      }
      if (!c)
        c = a;
      else if (!same_const(c, a, ir_is_float(i->ty))) {
        bottom = true;
        break;
      }
    }
    if (bottom)
      set_lattice(i, LAT_BOTTOM, 0, 0);
    else if (c)
      set_lattice(i, LAT_CONST, ir_is_float(i->ty) ? 0 : ival(c), ir_is_float(i->ty) ? fval(c) : 0);
    return;
  }
  case IR_JMP:
    mark_edge(b, i->blocks[0]);
    return;
  case IR_BR: {
    int l = lat(i->args[0]);
    if (l == LAT_TOP)
      return;
    if (l == LAT_CONST) {
      mark_edge(b, ival(i->args[0]) ? i->blocks[0] : i->blocks[1]);
      return;
    }
    mark_edge(b, i->blocks[0]);
    mark_edge(b, i->blocks[1]);
    return;
  }
  case IR_SWITCH: {
    int l = lat(i->args[0]);
    if (l == LAT_TOP)
      return;
    if (l == LAT_CONST) {
      int64_t v = ival(i->args[0]);
      Block *t = i->blocks[0];
      for (int k = 1; k < i->nblocks; k++)
        if (norm(i->args[0]->ty, i->case_vals[k - 1]) == v)
          t = i->blocks[k];
      mark_edge(b, t);
      return;
    }
    for (int k = 0; k < i->nblocks; k++)
      mark_edge(b, i->blocks[k]);
    return;
  }
  default:
    break;
  }
  if (i->ty == IR_VOID)
    return;
  if (!ir_is_pure(i) || ir_is_floating(i)) {
    set_lattice(i, LAT_BOTTOM, 0, 0);
    return;
  }
  for (int k = 0; k < i->nargs; k++) {
    int l = lat(i->args[k]);
    if (l == LAT_BOTTOM) {
      set_lattice(i, LAT_BOTTOM, 0, 0);
      return;
    }
    if (l == LAT_TOP)
      return;
  }
  int64_t iv = 0;
  double fv = 0;
  if (evaluate(i, &iv, &fv))
    set_lattice(i, LAT_CONST, iv, fv);
  else
    set_lattice(i, LAT_BOTTOM, 0, 0);
}

bool opt_sccp(Func *f) {
  ir_compute_preds(f);
  ir_compute_uses(f);
  int total = 0;
  edge_base = xalloc(sizeof(int) * (f->next_block_id + 1));
  for (Block *b = f->entry; b; b = b->next) {
    edge_base[b->id] = total;
    total += b->npreds;
    b->mark = false;
    for (Inst *i = b->first; i; i = i->next)
      i->lattice = LAT_TOP;
  }
  edge_exec = xalloc(sizeof(bool) * (total + 1));
  // Phi operand -> incoming edge index, computed once: a dispatch block
  // can have hundreds of predecessors, and phis are re-evaluated often.
  int *slot_of = xalloc(sizeof(int) * (f->next_block_id + 1));
  for (Block *b = f->entry; b; b = b->next) {
    if (!b->first || b->first->op != IR_PHI)
      continue;
    for (int p = 0; p < b->npreds; p++)
      slot_of[b->preds[p]->id] = p + 1;
    for (Inst *i = b->first; i && i->op == IR_PHI; i = i->next) {
      i->arg_edge = xalloc(sizeof(int) * (i->nargs + 1));
      for (int k = 0; k < i->nargs; k++)
        i->arg_edge[k] = slot_of[i->blocks[k]->id] - 1;
    }
    for (int p = 0; p < b->npreds; p++)
      slot_of[b->preds[p]->id] = 0;
  }
  flow_work.len = 0;
  ssa_work.len = 0;
  f->entry->mark = true;
  vec_push(&flow_work, f->entry);

  while (flow_work.len || ssa_work.len) {
    if (flow_work.len) {
      Block *b = flow_work.data[--flow_work.len];
      for (Inst *i = b->first; i; i = i->next)
        visit(i);
      continue;
    }
    Inst *i = ssa_work.data[--ssa_work.len];
    for (int k = 0; k < i->nusers; k++)
      visit(i->users[k]);
  }

  // Rewrite.
  bool changed = false;
  for (Block *b = f->entry; b; b = b->next) {
    if (!b->mark)
      continue;
    for (Inst *i = b->first; i;) {
      Inst *next = i->next;
      if (i->lattice == LAT_CONST && i->ty != IR_VOID && !ir_has_side_effects(i) &&
          i->op != IR_CALL) {
        Inst *c = ir_is_float(i->ty) ? ir_fconst(f, i->ty, i->lat_f) : ir_iconst(f, i->ty, i->lat_i);
        ir_unlink(i);
        ir_replace(i, c);
        changed = true;
      }
      i = next;
    }
    Inst *t = b->last;
    if ((t->op == IR_BR || t->op == IR_SWITCH) && lat(ir_resolve(t->args[0])) == LAT_CONST) {
      // Keep only the executable edge.
      Block *target = NULL;
      for (int k = 0; k < t->nblocks; k++) {
        Block *s = t->blocks[k];
        int idx = ir_pred_index(s, b);
        if (edge_exec[edge_base[s->id] + idx])
          target = s;
      }
      if (!target)
        continue;
      for (int k = 0; k < t->nblocks; k++)
        if (t->blocks[k] != target)
          ir_phi_remove_pred(t->blocks[k], b);
      t->op = IR_JMP;
      t->nargs = 0;
      t->blocks[0] = target;
      t->nblocks = 1;
      changed = true;
    }
  }
  if (changed) {
    ir_resolve_all(f);
    ir_compute_preds(f);
  }
  for (Block *b = f->entry; b; b = b->next)
    b->mark = false;
  return changed;
}
