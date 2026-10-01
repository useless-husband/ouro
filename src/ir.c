// The intermediate representation: SSA form over a control-flow graph.
//
// Every instruction that produces a value *is* that value; operands are
// pointers to the defining instruction.  Constants and symbol addresses
// are "floating" instructions that belong to no block; the instruction
// selector materialises them at each use, which keeps their live ranges
// to a single instruction.
//
// Instructions are replaced lazily: `ir_replace(old, new)` records a
// forwarding pointer, and operands are redirected by `ir_resolve_all`.

#include "ouro.h"

const char *ir_op_name[] = {
    "nop",     "iconst", "fconst", "gaddr",  "alloca",  "param",   "argaddr", "copy",
    "add",     "sub",    "mul",    "sdiv",   "udiv",    "srem",    "urem",    "and",
    "or",      "xor",    "shl",    "lshr",   "ashr",    "neg",     "not",     "fadd",
    "fsub",    "fmul",   "fdiv",   "fneg",   "eq",      "ne",      "slt",     "sle",
    "sgt",     "sge",    "ult",    "ule",    "ugt",     "uge",     "feq",     "fne",
    "flt",     "fle",    "fgt",    "fge",    "sext8",   "sext16",  "zext8",   "zext16",
    "sext32",  "zext32", "trunc",  "sitof",  "uitof",   "ftosi",   "ftoui",   "fext",
    "ftrunc",  "bitcast", "load",  "store",  "memcpy",  "memzero", "call",    "phi",
    "jmp",     "br",     "switch", "ret",    "unreachable",
};

const char *ir_type_name[] = {"void", "i8", "i16", "i32", "i64", "f32", "f64"};

static int func_counter;

Func *ir_new_func(Sym *fn) {
  Func *f = xalloc(sizeof(Func));
  f->name = fn->asm_name;
  f->sym = fn;
  f->id = func_counter++;
  return f;
}

Block *ir_new_block(Func *f) {
  Block *b = xalloc(sizeof(Block));
  b->id = f->next_block_id++;
  return b;
}

void ir_append_block(Func *f, Block *b) {
  b->prev = f->last;
  b->next = NULL;
  if (f->last)
    f->last->next = b;
  else
    f->entry = b;
  f->last = b;
  f->nblocks++;
}

void ir_insert_block_after(Func *f, Block *pos, Block *b) {
  b->prev = pos;
  b->next = pos->next;
  if (pos->next)
    pos->next->prev = b;
  else
    f->last = b;
  pos->next = b;
  f->nblocks++;
}

void ir_remove_block(Func *f, Block *b) {
  if (b->prev)
    b->prev->next = b->next;
  else
    f->entry = b->next;
  if (b->next)
    b->next->prev = b->prev;
  else
    f->last = b->prev;
  b->prev = b->next = NULL;
  f->nblocks--;
}

Inst *ir_new(Func *f, int op, int ty, int nargs) {
  Inst *i = xalloc(sizeof(Inst));
  i->op = (short)op;
  i->ty = (char)ty;
  i->id = f->next_inst_id++;
  i->nargs = nargs;
  i->vreg = -1;
  if (nargs)
    i->args = xalloc(sizeof(Inst *) * nargs);
  return i;
}

void ir_append(Block *b, Inst *i) {
  i->bb = b;
  i->prev = b->last;
  i->next = NULL;
  if (b->last)
    b->last->next = i;
  else
    b->first = i;
  b->last = i;
}

void ir_insert_before(Inst *pos, Inst *i) {
  Block *b = pos->bb;
  i->bb = b;
  i->next = pos;
  i->prev = pos->prev;
  if (pos->prev)
    pos->prev->next = i;
  else
    b->first = i;
  pos->prev = i;
}

void ir_insert_front(Block *b, Inst *i) {
  if (b->first)
    ir_insert_before(b->first, i);
  else
    ir_append(b, i);
}

void ir_unlink(Inst *i) {
  Block *b = i->bb;
  if (!b)
    return;
  if (i->prev)
    i->prev->next = i->next;
  else
    b->first = i->next;
  if (i->next)
    i->next->prev = i->prev;
  else
    b->last = i->prev;
  i->bb = NULL;
  i->prev = i->next = NULL;
}

Inst *ir_iconst(Func *f, int ty, int64_t v) {
  Inst *i = ir_new(f, IR_ICONST, ty, 0);
  // 32-bit constants are stored sign-extended so that equal values have
  // equal representations.
  i->ival = ty == IR_I32 ? (int64_t)(int32_t)v : v;
  return i;
}

Inst *ir_fconst(Func *f, int ty, double v) {
  Inst *i = ir_new(f, IR_FCONST, ty, 0);
  i->fval = ty == IR_F32 ? (double)(float)v : v;
  return i;
}

bool ir_is_terminator(int op) {
  return op == IR_JMP || op == IR_BR || op == IR_SWITCH || op == IR_RET || op == IR_UNREACHABLE;
}

bool ir_is_float(int ty) {
  return ty == IR_F32 || ty == IR_F64;
}

bool ir_is_floating(Inst *i) {
  return i->op == IR_ICONST || i->op == IR_FCONST || i->op == IR_GADDR;
}

bool ir_has_side_effects(Inst *i) {
  switch (i->op) {
  case IR_STORE:
  case IR_MEMCPY:
  case IR_MEMZERO:
  case IR_CALL:
  case IR_JMP:
  case IR_BR:
  case IR_SWITCH:
  case IR_RET:
  case IR_UNREACHABLE:
    return true;
  case IR_LOAD:
    return i->is_volatile;
  default:
    return false;
  }
}

// Instructions whose result depends only on their operands (candidates
// for folding and common subexpression elimination).  Division may trap
// on other targets but not on arm64, where x/0 is 0.
bool ir_is_pure(Inst *i) {
  switch (i->op) {
  case IR_LOAD:
  case IR_STORE:
  case IR_MEMCPY:
  case IR_MEMZERO:
  case IR_CALL:
  case IR_PHI:
  case IR_ALLOCA:
  case IR_PARAM:
  case IR_ARGADDR:
  case IR_NOP:
    return false;
  default:
    return !ir_is_terminator(i->op);
  }
}

int ir_size(int ty) {
  switch (ty) {
  case IR_I8:
    return 1;
  case IR_I16:
    return 2;
  case IR_I32:
  case IR_F32:
    return 4;
  case IR_I64:
  case IR_F64:
    return 8;
  default:
    return 0;
  }
}

int ir_nsucc(Block *b) {
  Inst *t = b->last;
  if (!t || !ir_is_terminator(t->op))
    return 0;
  return t->nblocks;
}

Block *ir_succ(Block *b, int i) {
  return b->last->blocks[i];
}

static void add_pred(Block *b, Block *p) {
  for (int i = 0; i < b->npreds; i++)
    if (b->preds[i] == p)
      return;
  if (b->npreds == b->pred_cap) {
    int cap = b->pred_cap ? b->pred_cap * 2 : 4;
    b->preds = xgrow(b->preds, sizeof(Block *) * b->npreds, sizeof(Block *) * cap);
    b->pred_cap = cap;
  }
  b->preds[b->npreds++] = p;
}

// Predecessor lists hold each predecessor block once, even if it reaches
// the block along several edges (a switch with several cases going to
// the same place); phi nodes likewise have one entry per predecessor.
void ir_compute_preds(Func *f) {
  for (Block *b = f->entry; b; b = b->next)
    b->npreds = 0;
  for (Block *b = f->entry; b; b = b->next) {
    int n = ir_nsucc(b);
    for (int i = 0; i < n; i++)
      add_pred(ir_succ(b, i), b);
  }
}

int ir_pred_index(Block *b, Block *pred) {
  for (int i = 0; i < b->npreds; i++)
    if (b->preds[i] == pred)
      return i;
  return -1;
}

Inst *ir_resolve(Inst *i) {
  while (i && i->fwd)
    i = i->fwd;
  return i;
}

void ir_replace(Inst *old, Inst *with) {
  ASSERT(old != with);
  old->fwd = with;
}

void ir_resolve_all(Func *f) {
  for (Block *b = f->entry; b; b = b->next)
    for (Inst *i = b->first; i; i = i->next)
      for (int k = 0; k < i->nargs; k++)
        i->args[k] = ir_resolve(i->args[k]);
}

// Builds the user lists of every instruction in a block (floating
// constants get none: they are never shared in a way that matters).
void ir_compute_uses(Func *f) {
  for (Block *b = f->entry; b; b = b->next)
    for (Inst *i = b->first; i; i = i->next) {
      i->nuses = 0;
      i->nusers = 0;
    }
  for (Block *b = f->entry; b; b = b->next)
    for (Inst *i = b->first; i; i = i->next)
      for (int k = 0; k < i->nargs; k++)
        if (i->args[k]->bb)
          i->args[k]->nuses++;
  for (Block *b = f->entry; b; b = b->next)
    for (Inst *i = b->first; i; i = i->next)
      i->users = i->nuses ? xalloc(sizeof(Inst *) * i->nuses) : NULL;
  for (Block *b = f->entry; b; b = b->next)
    for (Inst *i = b->first; i; i = i->next)
      for (int k = 0; k < i->nargs; k++) {
        Inst *a = i->args[k];
        if (a->bb)
          a->users[a->nusers++] = i;
      }
}

// Removes the phi entries that come from `pred`.
void ir_phi_remove_pred(Block *b, Block *pred) {
  for (Inst *i = b->first; i && i->op == IR_PHI; i = i->next) {
    int out = 0;
    for (int k = 0; k < i->nargs; k++) {
      if (i->blocks[k] == pred)
        continue;
      i->args[out] = i->args[k];
      i->blocks[out] = i->blocks[k];
      out++;
    }
    i->nargs = out;
    i->nblocks = out;
  }
}

void ir_phi_replace_pred(Block *b, Block *old, Block *with) {
  for (Inst *i = b->first; i && i->op == IR_PHI; i = i->next)
    for (int k = 0; k < i->nargs; k++)
      if (i->blocks[k] == old)
        i->blocks[k] = with;
}

void ir_renumber(Func *f) {
  int n = 0;
  int nb = 0;
  for (Block *b = f->entry; b; b = b->next) {
    b->id = nb++;
    for (Inst *i = b->first; i; i = i->next)
      i->id = n++;
  }
  f->next_inst_id = n;
  f->next_block_id = nb;
}

//
// Printing (--dump-ir)
//

static void print_operand(FILE *out, Inst *a) {
  if (!a) {
    fprintf(out, "<null>");
    return;
  }
  switch (a->op) {
  case IR_ICONST:
    fprintf(out, "%lld", (long long)a->ival);
    return;
  case IR_FCONST:
    fprintf(out, "%.17g", a->fval);
    return;
  case IR_GADDR:
    if (a->ival)
      fprintf(out, "@%s%+lld", a->sym->asm_name, (long long)a->ival);
    else
      fprintf(out, "@%s", a->sym->asm_name);
    return;
  default:
    fprintf(out, "%%%d", a->id);
  }
}

static void print_inst(FILE *out, Inst *i) {
  fprintf(out, "  ");
  if (i->ty != IR_VOID)
    fprintf(out, "%%%d = ", i->id);
  switch (i->op) {
  case IR_ALLOCA:
    fprintf(out, "alloca %lld, align %d", (long long)i->ival, i->align);
    if (i->var_name)
      fprintf(out, "  ; %s", i->var_name->name);
    fprintf(out, "\n");
    return;
  case IR_PARAM:
    fprintf(out, "param %s %s%lld\n", ir_type_name[(int)i->ty], ir_is_float(i->ty) ? "v" : "x",
            (long long)i->ival);
    return;
  case IR_ARGADDR:
    fprintf(out, "argaddr %lld\n", (long long)i->ival);
    return;
  case IR_LOAD:
    fprintf(out, "load%s %s%s ", i->is_volatile ? " volatile" : "", ir_type_name[(int)i->memty],
            (i->memty == IR_I8 || i->memty == IR_I16) ? (i->is_signed ? "s" : "u") : "");
    print_operand(out, i->args[0]);
    fprintf(out, "\n");
    return;
  case IR_STORE:
    fprintf(out, "store%s %s ", i->is_volatile ? " volatile" : "", ir_type_name[(int)i->memty]);
    print_operand(out, i->args[0]);
    fprintf(out, ", ");
    print_operand(out, i->args[1]);
    fprintf(out, "\n");
    return;
  case IR_MEMCPY:
  case IR_MEMZERO:
    fprintf(out, "%s ", ir_op_name[i->op]);
    for (int k = 0; k < i->nargs; k++) {
      print_operand(out, i->args[k]);
      fprintf(out, ", ");
    }
    fprintf(out, "%lld\n", (long long)i->ival);
    return;
  case IR_PHI:
    fprintf(out, "phi %s ", ir_type_name[(int)i->ty]);
    for (int k = 0; k < i->nargs; k++) {
      fprintf(out, k ? ", [" : "[");
      print_operand(out, i->args[k]);
      fprintf(out, ", bb%d]", i->blocks[k]->id);
    }
    fprintf(out, "\n");
    return;
  case IR_JMP:
    fprintf(out, "jmp bb%d\n", i->blocks[0]->id);
    return;
  case IR_BR:
    fprintf(out, "br ");
    print_operand(out, i->args[0]);
    fprintf(out, ", bb%d, bb%d\n", i->blocks[0]->id, i->blocks[1]->id);
    return;
  case IR_SWITCH:
    fprintf(out, "switch ");
    print_operand(out, i->args[0]);
    fprintf(out, ", default bb%d", i->blocks[0]->id);
    for (int k = 1; k < i->nblocks; k++)
      fprintf(out, ", %lld: bb%d", (long long)i->case_vals[k - 1], i->blocks[k]->id);
    fprintf(out, "\n");
    return;
  case IR_CALL: {
    CallInfo *ci = i->call;
    int first = ci->indirect ? 1 : 0;
    fprintf(out, "call %s", i->ty != IR_VOID ? ir_type_name[(int)i->ty] : "void");
    fprintf(out, " ");
    if (ci->indirect)
      print_operand(out, i->args[0]);
    else
      fprintf(out, "@%s", i->sym->asm_name);
    fprintf(out, "(");
    for (int k = 0; k < ci->nlocs; k++) {
      ArgLoc *l = &ci->locs[k];
      if (k)
        fprintf(out, ", ");
      if (l->kind == LOC_GPR)
        fprintf(out, "x%d=", l->reg);
      else if (l->kind == LOC_FPR)
        fprintf(out, "v%d=", l->reg);
      else
        fprintf(out, "[sp+%d]=", l->off);
      print_operand(out, i->args[first + k]);
    }
    fprintf(out, ")");
    if (ci->ret_kind == RET_GPRS || ci->ret_kind == RET_FPRS) {
      fprintf(out, " -> ");
      print_operand(out, i->args[i->nargs - 1]);
    }
    fprintf(out, "\n");
    return;
  }
  default:
    break;
  }
  fprintf(out, "%s", ir_op_name[i->op]);
  if (i->ty != IR_VOID)
    fprintf(out, " %s", ir_type_name[(int)i->ty]);
  for (int k = 0; k < i->nargs; k++) {
    fprintf(out, k ? ", " : " ");
    print_operand(out, i->args[k]);
  }
  fprintf(out, "\n");
}

void ir_dump(FILE *out, Func *f) {
  ir_renumber(f);
  fprintf(out, "function %s {\n", f->name);
  for (Block *b = f->entry; b; b = b->next) {
    fprintf(out, "bb%d:", b->id);
    if (b->npreds) {
      fprintf(out, "  ; preds");
      for (int k = 0; k < b->npreds; k++)
        fprintf(out, " bb%d", b->preds[k]->id);
    }
    fprintf(out, "\n");
    for (Inst *i = b->first; i; i = i->next)
      print_inst(out, i);
  }
  fprintf(out, "}\n\n");
}

//
// Verifier: structural invariants that every pass must preserve.
// Enabled with OURO_VERIFY=1 (the test suite does this).
//

void ir_verify(Func *f, const char *after) {
  static int enabled = -1;
  if (enabled < 0) {
    char *e = getenv("OURO_VERIFY");
    enabled = e && str_eq(e, "1");
  }
  if (!enabled)
    return;
  ir_compute_preds(f);
  for (Block *b = f->entry; b; b = b->next) {
    if (!b->last || !ir_is_terminator(b->last->op))
      ICE("%s: %s: block bb%d does not end in a terminator", after, f->name, b->id);
    bool phis_done = false;
    for (Inst *i = b->first; i; i = i->next) {
      if (i->bb != b)
        ICE("%s: %s: instruction %%%d has the wrong block", after, f->name, i->id);
      if (i->fwd)
        ICE("%s: %s: replaced instruction %%%d still in a block", after, f->name, i->id);
      if (ir_is_terminator(i->op) && i != b->last)
        ICE("%s: %s: terminator in the middle of bb%d", after, f->name, b->id);
      if (i->op == IR_PHI) {
        if (phis_done)
          ICE("%s: %s: phi after a non-phi in bb%d", after, f->name, b->id);
        if (i->nargs != b->npreds)
          ICE("%s: %s: phi %%%d has %d entries but bb%d has %d predecessors", after, f->name,
              i->id, i->nargs, b->id, b->npreds);
        for (int k = 0; k < i->nargs; k++)
          if (ir_pred_index(b, i->blocks[k]) < 0)
            ICE("%s: %s: phi %%%d names bb%d, which is not a predecessor", after, f->name,
                i->id, i->blocks[k]->id);
      } else {
        phis_done = true;
      }
      for (int k = 0; k < i->nargs; k++) {
        Inst *a = i->args[k];
        if (!a)
          ICE("%s: %s: %%%d has a null operand", after, f->name, i->id);
        if (a->fwd)
          ICE("%s: %s: %%%d uses replaced %%%d", after, f->name, i->id, a->id);
        if (!ir_is_floating(a) && !a->bb)
          ICE("%s: %s: %%%d uses %%%d, which is not in any block", after, f->name, i->id, a->id);
      }
    }
    for (int k = 0; k < ir_nsucc(b); k++)
      if (!ir_succ(b, k))
        ICE("%s: %s: bb%d has a null successor", after, f->name, b->id);
  }
}
