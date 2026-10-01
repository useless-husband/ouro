// Memory and loop optimisations: redundant load elimination with
// store-to-load forwarding, and loop-invariant code motion.

#include "ouro.h"

//
// Redundant loads, within a basic block.
//
// The pass remembers, for each address seen in the block, the value
// last loaded from or stored to it.  An address is a base value plus a
// constant offset.  A store forgets every remembered location it may
// overlap: two locations are known to be distinct only if they have the
// same base and disjoint byte ranges, or if their bases are different
// named objects (two globals, two stack slots, or one of each).  Calls,
// block copies and volatile accesses forget everything.
//

typedef struct {
  Inst *base;
  int64_t off;
  int size;
  int memty;
  bool is_signed; // for narrow loads: how the remembered value was extended
  bool from_store;
  Inst *val;
} MemFact;

static void split_addr(Inst *a, Inst **base, int64_t *off) {
  *off = 0;
  if (a->op == IR_ADD && a->ty == IR_I64 && a->args[1]->op == IR_ICONST) {
    *off = a->args[1]->ival;
    a = a->args[0];
  }
  if (a->op == IR_GADDR) {
    *off += a->ival;
  }
  *base = a;
}

static bool same_base(Inst *a, Inst *b) {
  if (a == b)
    return true;
  return a->op == IR_GADDR && b->op == IR_GADDR && a->sym == b->sym;
}

static bool named_object(Inst *a) {
  return a->op == IR_GADDR || a->op == IR_ALLOCA;
}

static bool may_alias(MemFact *f, Inst *base, int64_t off, int size) {
  if (same_base(f->base, base))
    return off < f->off + f->size && f->off < off + size;
  if (named_object(f->base) && named_object(base))
    return false; // distinct objects
  return true;
}

static Inst *extension(Func *f, Inst *before, int memty, bool is_signed, Inst *v) {
  int op;
  if (memty == IR_I8)
    op = is_signed ? IR_SEXT8 : IR_ZEXT8;
  else if (memty == IR_I16)
    op = is_signed ? IR_SEXT16 : IR_ZEXT16;
  else
    return v;
  Inst *x = ir_new(f, op, IR_I32, 1);
  x->args[0] = v;
  ir_insert_before(before, x);
  return x;
}

bool opt_load_forward(Func *f) {
  bool changed = false;
  MemFact *facts = xalloc(sizeof(MemFact) * 64);
  for (Block *b = f->entry; b; b = b->next) {
    int n = 0;
    for (Inst *i = b->first; i;) {
      Inst *next = i->next;
      for (int k = 0; k < i->nargs; k++)
        i->args[k] = ir_resolve(i->args[k]);
      if (i->op == IR_LOAD && !i->is_volatile) {
        Inst *base;
        int64_t off;
        split_addr(i->args[0], &base, &off);
        int size = ir_size(i->memty);
        Inst *found = NULL;
        for (int k = 0; k < n; k++) {
          MemFact *m = &facts[k];
          if (same_base(m->base, base) && m->off == off && m->memty == i->memty) {
            if (m->from_store || m->is_signed == i->is_signed || ir_size(i->memty) >= 4)
              found = m->from_store ? extension(f, i, i->memty, i->is_signed, m->val) : m->val;
            break;
          }
        }
        if (found) {
          ir_unlink(i);
          ir_replace(i, found);
          changed = true;
        } else if (n < 64) {
          MemFact *m = &facts[n++];
          m->base = base;
          m->off = off;
          m->size = size;
          m->memty = i->memty;
          m->is_signed = i->is_signed;
          m->from_store = false;
          m->val = i;
        }
      } else if (i->op == IR_STORE) {
        Inst *base;
        int64_t off;
        split_addr(i->args[1], &base, &off);
        int size = ir_size(i->memty);
        int out = 0;
        for (int k = 0; k < n; k++)
          if (!may_alias(&facts[k], base, off, size))
            facts[out++] = facts[k];
        n = out;
        if (!i->is_volatile && n < 64) {
          MemFact *m = &facts[n++];
          m->base = base;
          m->off = off;
          m->size = size;
          m->memty = i->memty;
          m->is_signed = false;
          m->from_store = true;
          m->val = i->args[0];
        }
      } else if (i->op == IR_CALL || i->op == IR_MEMCPY || i->op == IR_MEMZERO ||
                 (i->op == IR_LOAD && i->is_volatile)) {
        n = 0;
      }
      i = next;
    }
  }
  if (changed)
    ir_resolve_all(f);
  ir_verify(f, "load-forward");
  return changed;
}

//
// Loop-invariant code motion.
//
// For each natural loop whose header has a single predecessor outside
// the loop, pure instructions whose operands are all defined outside the
// loop move to the end of that predecessor (a preheader block is created
// when the predecessor has other successors).  Hoisting a pure
// instruction is always safe here: none of them can trap on arm64 (an
// integer division by zero yields 0), so evaluating one on a path where
// the loop does not run is harmless.  Inner loops go first, so an
// invariant can move out of several loops in turn.
//

typedef struct {
  Block *header;
  bool *in;  // indexed by block id
  int size;
} Loop;

static int cmp_loop_size(const void *a, const void *b) {
  const Loop *x = *(const Loop **)a;
  const Loop *y = *(const Loop **)b;
  if (x->size != y->size)
    return x->size < y->size ? -1 : 1;
  return x->header->id < y->header->id ? -1 : x->header->id > y->header->id;
}

static bool defined_outside(Inst *v, Loop *l) {
  if (!v->bb)
    return true; // constant or symbol address
  return !l->in[v->bb->id];
}

bool opt_licm(Func *f) {
  ir_renumber(f);
  opt_dominators(f);
  // Room for one preheader per loop: membership arrays never regrow.
  int nloops = 0;
  for (Block *h = f->entry; h; h = h->next)
    for (int p = 0; p < h->npreds; p++)
      if (dominates(h, h->preds[p])) {
        nloops++;
        break;
      }
  int nb = f->next_block_id + nloops + 1;
  Vec loops = {0};
  for (Block *h = f->entry; h; h = h->next) {
    Loop *l = NULL;
    for (int p = 0; p < h->npreds; p++) {
      Block *latch = h->preds[p];
      if (!dominates(h, latch))
        continue;
      if (!l) {
        l = xalloc(sizeof(Loop));
        l->header = h;
        l->in = xalloc(sizeof(bool) * (nb + 1));
        l->in[h->id] = true;
        l->size = 1;
        vec_push(&loops, l);
      }
      // blocks that reach the latch without passing the header
      Vec stack = {0};
      if (!l->in[latch->id]) {
        l->in[latch->id] = true;
        l->size++;
        vec_push(&stack, latch);
      }
      while (stack.len) {
        Block *x = stack.data[--stack.len];
        for (int q = 0; q < x->npreds; q++) {
          Block *y = x->preds[q];
          if (!l->in[y->id]) {
            l->in[y->id] = true;
            l->size++;
            vec_push(&stack, y);
          }
        }
      }
    }
  }
  if (!loops.len)
    return false;
  qsort(loops.data, loops.len, sizeof(void *), cmp_loop_size);

  bool changed = false;
  for (int li = 0; li < loops.len; li++) {
    Loop *l = loops.data[li];
    Block *h = l->header;
    Block *outside = NULL;
    int nout = 0;
    for (int p = 0; p < h->npreds; p++)
      if (!l->in[h->preds[p]->id]) {
        outside = h->preds[p];
        nout++;
      }
    if (nout != 1)
      continue;
    // Find the invariant instructions first; build the preheader only
    // if there is something to move.
    Vec inv = {0};
    bool progress = true;
    for (Block *b = f->entry; b; b = b->next)
      for (Inst *i = b->first; i; i = i->next)
        i->mark = false;
    while (progress) {
      progress = false;
      for (Block *b = f->entry; b; b = b->next) {
        if (!l->in[b->id])
          continue;
        for (Inst *i = b->first; i; i = i->next) {
          if (i->mark || !ir_is_pure(i) || i->op == IR_PHI || i->ty == IR_VOID ||
              ir_is_floating(i))
            continue;
          bool ok = true;
          for (int k = 0; k < i->nargs && ok; k++) {
            Inst *a = i->args[k];
            if (!defined_outside(a, l) && !a->mark)
              ok = false;
          }
          if (ok) {
            i->mark = true;
            vec_push(&inv, i);
            progress = true;
          }
        }
      }
    }
    if (!inv.len)
      continue;
    Block *pre = outside;
    if (ir_nsucc(outside) != 1) {
      pre = ir_new_block(f);
      Inst *j = ir_new(f, IR_JMP, IR_VOID, 0);
      j->blocks = xalloc(sizeof(Block *));
      j->blocks[0] = h;
      j->nblocks = 1;
      ir_append(pre, j);
      Inst *t = outside->last;
      for (int k = 0; k < t->nblocks; k++)
        if (t->blocks[k] == h)
          t->blocks[k] = pre;
      ir_phi_replace_pred(h, outside, pre);
      ir_insert_block_after(f, outside, pre);
      ir_compute_preds(f);
      ASSERT(pre->id < nb);
      // The new block belongs to every enclosing loop that contains the
      // outside predecessor.
      for (int lj = li + 1; lj < loops.len; lj++) {
        Loop *o = loops.data[lj];
        o->in[pre->id] = o->in[outside->id];
      }
    }
    // Move in dependency order (the order they were found in).
    for (int k = 0; k < inv.len; k++) {
      Inst *i = inv.data[k];
      ir_unlink(i);
      ir_insert_before(pre->last, i);
      i->mark = false;
    }
    changed = true;
  }
  ir_verify(f, "licm");
  return changed;
}
