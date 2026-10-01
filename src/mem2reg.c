// mem2reg: turns stack slots into SSA values (Cytron et al., "Efficiently
// Computing Static Single Assignment Form and the Control Dependence
// Graph", 1991).
//
// A slot is promoted when its address is used only as the address of
// loads and stores of one memory type, none volatile.  Phi nodes go at
// the iterated dominance frontier of the blocks that store to it; a walk
// of the dominator tree then replaces each load by the value reaching it.
//
// A load of a narrow integer from a promoted slot is replaced by an
// explicit sign or zero extension of the reaching value, with the
// signedness of the load.  That keeps the canonical form of I32 values
// even when one slot is accessed as both char and unsigned char; the
// combine pass removes the extensions that turn out to be redundant.

#include "ouro.h"

typedef struct {
  Inst *slot;
  int memty;
  int valty;
  bool ok;
  Inst **stack; // reaching definitions during renaming
  int sp;
  int cap;
} Var;

static Var *vars;
static int nvars;

static Var *var_of(Inst *addr) {
  if (addr->op != IR_ALLOCA || addr->aux < 0 || addr->aux >= nvars)
    return NULL;
  Var *v = &vars[addr->aux];
  return v->slot == addr ? v : NULL;
}

static int value_type(int memty) {
  return (memty == IR_I8 || memty == IR_I16) ? IR_I32 : memty;
}

static void note_access(Var *v, int memty) {
  if (v->memty == IR_VOID) {
    v->memty = memty;
    if (ir_size(memty) != v->slot->ival)
      v->ok = false;
  } else if (v->memty != memty) {
    v->ok = false;
  }
}

static void push(Var *v, Inst *val) {
  if (v->sp == v->cap) {
    int cap = v->cap ? v->cap * 2 : 8;
    v->stack = xgrow(v->stack, sizeof(Inst *) * v->cap, sizeof(Inst *) * cap);
    v->cap = cap;
  }
  v->stack[v->sp++] = val;
}

static Inst *undef(Func *f, int ty) {
  // Reading an uninitialised variable is undefined; zero is a fine value.
  return ir_is_float(ty) ? ir_fconst(f, ty, 0) : ir_iconst(f, ty, 0);
}

static Inst *top(Func *f, Var *v) {
  return v->sp ? v->stack[v->sp - 1] : undef(f, v->valty);
}

static Inst *extend_for_load(Func *f, Inst *load, Inst *val) {
  int op;
  if (load->memty == IR_I8)
    op = load->is_signed ? IR_SEXT8 : IR_ZEXT8;
  else if (load->memty == IR_I16)
    op = load->is_signed ? IR_SEXT16 : IR_ZEXT16;
  else
    return val;
  Inst *x = ir_new(f, op, IR_I32, 1);
  x->args[0] = val;
  ir_insert_before(load, x);
  return x;
}

static void rename_block(Func *f, Block *b, int *pushed) {
  for (Inst *i = b->first; i;) {
    Inst *next = i->next;
    if (i->op == IR_PHI && i->aux >= 0 && i->mark) {
      push(&vars[i->aux], i);
      pushed[i->aux]++;
    } else if (i->op == IR_LOAD) {
      Var *v = var_of(i->args[0]);
      if (v && v->ok) {
        Inst *val = extend_for_load(f, i, top(f, v));
        ir_unlink(i);
        ir_replace(i, val);
      }
    } else if (i->op == IR_STORE) {
      Var *v = var_of(i->args[1]);
      if (v && v->ok) {
        push(v, ir_resolve(i->args[0]));
        pushed[v - vars]++;
        ir_unlink(i);
      }
    }
    i = next;
  }
  for (int k = 0; k < ir_nsucc(b); k++) {
    Block *s = ir_succ(b, k);
    for (Inst *p = s->first; p && p->op == IR_PHI; p = p->next) {
      if (!(p->aux >= 0 && p->mark))
        continue;
      for (int j = 0; j < p->nargs; j++)
        if (p->blocks[j] == b)
          p->args[j] = top(f, &vars[p->aux]);
    }
  }
}

bool opt_mem2reg(Func *f) {
  if (f->no_promote)
    return false;
  opt_dominators(f);

  nvars = 0;
  for (Inst *i = f->entry->first; i; i = i->next)
    if (i->op == IR_ALLOCA)
      nvars++;
  if (!nvars)
    return false;
  vars = xalloc(sizeof(Var) * nvars);
  int n = 0;
  for (Inst *i = f->entry->first; i; i = i->next)
    if (i->op == IR_ALLOCA) {
      i->aux = n;
      vars[n].slot = i;
      vars[n].memty = IR_VOID;
      vars[n].ok = true;
      n++;
    }

  // Which slots qualify.
  for (Block *b = f->entry; b; b = b->next) {
    for (Inst *i = b->first; i; i = i->next) {
      i->mark = false;
      for (int k = 0; k < i->nargs; k++) {
        Var *v = var_of(i->args[k]);
        if (!v)
          continue;
        if (i->op == IR_LOAD && k == 0 && !i->is_volatile)
          note_access(v, i->memty);
        else if (i->op == IR_STORE && k == 1 && !i->is_volatile && i->args[0] != i->args[1])
          note_access(v, i->memty);
        else
          v->ok = false;
      }
    }
  }
  int npromote = 0;
  for (int k = 0; k < nvars; k++) {
    // A slot that is never accessed is left for DCE.
    if (vars[k].ok && vars[k].memty != IR_VOID) {
      vars[k].valty = value_type(vars[k].memty);
      npromote++;
    } else {
      vars[k].ok = false;
    }
  }
  if (!npromote)
    return false;

  // Phi placement at the iterated dominance frontier of the stores.
  Block **work = xalloc(sizeof(Block *) * (f->nblocks + 1));
  int *has_phi = xalloc(sizeof(int) * (f->next_block_id + 1));
  int *queued = xalloc(sizeof(int) * (f->next_block_id + 1));
  for (int k = 0; k < nvars; k++) {
    Var *v = &vars[k];
    if (!v->ok)
      continue;
    int stamp = k + 1;
    int nw = 0;
    for (Block *b = f->entry; b; b = b->next)
      for (Inst *i = b->first; i; i = i->next)
        if (i->op == IR_STORE && var_of(i->args[1]) == v && queued[b->id] != stamp) {
          queued[b->id] = stamp;
          work[nw++] = b;
        }
    while (nw) {
      Block *b = work[--nw];
      for (int d = 0; d < b->ndf; d++) {
        Block *y = b->df[d];
        if (has_phi[y->id] == stamp)
          continue;
        has_phi[y->id] = stamp;
        Inst *phi = ir_new(f, IR_PHI, v->valty, y->npreds);
        phi->blocks = xalloc(sizeof(Block *) * (y->npreds + 1));
        phi->nblocks = y->npreds;
        for (int p = 0; p < y->npreds; p++) {
          phi->blocks[p] = y->preds[p];
          phi->args[p] = NULL;
        }
        phi->aux = k;
        phi->mark = true;
        ir_insert_front(y, phi);
        if (queued[y->id] != stamp) {
          queued[y->id] = stamp;
          work[nw++] = y;
        }
      }
    }
  }

  // Renaming: a preorder walk of the dominator tree with an explicit
  // stack; each frame remembers how many values it pushed per variable.
  typedef struct {
    Block *b;
    int next_kid;
    int *pushed;
  } Frame;
  Frame *stack = xalloc(sizeof(Frame) * (f->nblocks + 1));
  int sp = 0;
  stack[sp].b = f->entry;
  stack[sp].next_kid = 0;
  stack[sp].pushed = xalloc(sizeof(int) * nvars);
  rename_block(f, f->entry, stack[sp].pushed);
  sp++;
  while (sp) {
    Frame *fr = &stack[sp - 1];
    if (fr->next_kid < fr->b->nkids) {
      Block *kid = fr->b->kids[fr->next_kid++];
      stack[sp].b = kid;
      stack[sp].next_kid = 0;
      stack[sp].pushed = xalloc(sizeof(int) * nvars);
      rename_block(f, kid, stack[sp].pushed);
      sp++;
      continue;
    }
    for (int k = 0; k < nvars; k++)
      vars[k].sp -= fr->pushed[k];
    sp--;
  }

  // Phi operands from blocks the walk never reached (unreachable
  // predecessors) get an undefined value.
  for (Block *b = f->entry; b; b = b->next)
    for (Inst *p = b->first; p && p->op == IR_PHI; p = p->next)
      for (int j = 0; j < p->nargs; j++)
        if (!p->args[j])
          p->args[j] = undef(f, p->ty);

  for (int k = 0; k < nvars; k++)
    if (vars[k].ok)
      ir_unlink(vars[k].slot);
  for (Block *b = f->entry; b; b = b->next)
    for (Inst *i = b->first; i; i = i->next) {
      if (i->op == IR_PHI)
        i->mark = false;
      i->aux = 0;
    }
  ir_resolve_all(f);
  ir_verify(f, "mem2reg");
  return true;
}
