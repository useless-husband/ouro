// Control-flow graph utilities: cleanup, critical edges, dominators and
// loop nesting.
//
// Invariant kept by every transformation here: the entry block has no
// predecessors (irgen.c starts the body in a separate block).

#include "ouro.h"

static void mark_reachable(Block *b) {
  // iterative DFS
  Vec stack = {0};
  vec_push(&stack, b);
  b->reachable = true;
  while (stack.len) {
    Block *x = stack.data[--stack.len];
    for (int i = 0; i < ir_nsucc(x); i++) {
      Block *s = ir_succ(x, i);
      if (!s->reachable) {
        s->reachable = true;
        vec_push(&stack, s);
      }
    }
  }
}

static bool remove_unreachable(Func *f) {
  for (Block *b = f->entry; b; b = b->next)
    b->reachable = false;
  mark_reachable(f->entry);
  bool changed = false;
  for (Block *b = f->entry; b;) {
    Block *next = b->next;
    if (!b->reachable) {
      for (int i = 0; i < ir_nsucc(b); i++) {
        Block *s = ir_succ(b, i);
        if (s->reachable)
          ir_phi_remove_pred(s, b);
      }
      ir_remove_block(f, b);
      changed = true;
    }
    b = next;
  }
  if (changed)
    ir_compute_preds(f);
  return changed;
}

// Replaces a phi that has a single incoming value (or only itself and
// one other value) by that value.
void opt_remove_trivial_phis(Func *f) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (Block *b = f->entry; b; b = b->next) {
      for (Inst *i = b->first; i && i->op == IR_PHI;) {
        Inst *next = i->next;
        Inst *same = NULL;
        bool trivial = true;
        for (int k = 0; k < i->nargs; k++) {
          Inst *a = ir_resolve(i->args[k]);
          if (a == i || a == same)
            continue;
          if (same && !(ir_is_floating(a) && ir_is_floating(same) && a->op == same->op &&
                        a->ival == same->ival && a->sym == same->sym && a->ty == same->ty &&
                        (a->op != IR_FCONST ||
                         memcmp(&a->fval, &same->fval, sizeof(double)) == 0))) {
            trivial = false;
            break;
          }
          same = a;
        }
        if (trivial && same) {
          ir_unlink(i);
          ir_replace(i, same);
          changed = true;
        }
        i = next;
      }
    }
    ir_resolve_all(f);
  }
}

// Turns `br c, X, X` and branches on constants into jumps.
static bool fold_branches(Func *f) {
  bool changed = false;
  for (Block *b = f->entry; b; b = b->next) {
    Inst *t = b->last;
    if (t->op == IR_BR) {
      Block *target = NULL;
      if (t->blocks[0] == t->blocks[1])
        target = t->blocks[0];
      else if (t->args[0]->op == IR_ICONST)
        target = t->args[0]->ival ? t->blocks[0] : t->blocks[1];
      if (!target)
        continue;
      Block *other = t->blocks[0] == target ? t->blocks[1] : t->blocks[0];
      if (other != target)
        ir_phi_remove_pred(other, b);
      t->op = IR_JMP;
      t->nargs = 0;
      t->blocks[0] = target;
      t->nblocks = 1;
      changed = true;
    } else if (t->op == IR_SWITCH && t->args[0]->op == IR_ICONST) {
      int64_t v = t->args[0]->ival;
      Block *target = t->blocks[0];
      for (int k = 1; k < t->nblocks; k++)
        if (t->case_vals[k - 1] == v)
          target = t->blocks[k];
      for (int k = 0; k < t->nblocks; k++)
        if (t->blocks[k] != target)
          ir_phi_remove_pred(t->blocks[k], b);
      t->op = IR_JMP;
      t->nargs = 0;
      t->blocks[0] = target;
      t->nblocks = 1;
      changed = true;
    } else if (t->op == IR_SWITCH) {
      // All targets the same: a jump.
      bool same = true;
      for (int k = 1; k < t->nblocks; k++)
        if (t->blocks[k] != t->blocks[0])
          same = false;
      if (same) {
        t->op = IR_JMP;
        t->nargs = 0;
        t->nblocks = 1;
        changed = true;
      }
    }
  }
  if (changed)
    ir_compute_preds(f);
  return changed;
}

// Merges a block into its only predecessor when that predecessor jumps
// to it unconditionally.
static bool merge_blocks(Func *f) {
  bool changed = false;
  for (Block *b = f->entry; b; b = b->next) {
    for (;;) {
      Inst *t = b->last;
      if (t->op != IR_JMP)
        break;
      Block *s = t->blocks[0];
      if (s == b || s == f->entry || s->npreds != 1)
        break;
      // Phis in a block with one predecessor have one entry each.
      for (Inst *i = s->first; i && i->op == IR_PHI;) {
        Inst *next = i->next;
        ir_unlink(i);
        ir_replace(i, i->args[0]);
        i = next;
      }
      ir_unlink(t);
      while (s->first) {
        Inst *i = s->first;
        ir_unlink(i);
        ir_append(b, i);
      }
      for (int k = 0; k < ir_nsucc(b); k++)
        ir_phi_replace_pred(ir_succ(b, k), s, b);
      ir_remove_block(f, s);
      ir_compute_preds(f);
      changed = true;
    }
  }
  if (changed)
    ir_resolve_all(f);
  return changed;
}

// Redirects jumps to blocks that contain nothing but a jump.
static bool thread_jumps(Func *f) {
  bool changed = false;
  for (Block *b = f->entry->next; b; b = b->next) {
    if (b->first != b->last || b->last->op != IR_JMP)
      continue;
    Block *target = b->last->blocks[0];
    if (target == b)
      continue;
    // Each predecessor P of b becomes a predecessor of target.  When
    // target has phis, a P that already reaches target directly is left
    // alone: one phi entry cannot hold two values.  The value flowing
    // from b is available in P because its definition dominates b.
    bool has_phi = target->first && target->first->op == IR_PHI;
    int npreds = b->npreds;
    Block **preds = xalloc(sizeof(Block *) * (npreds + 1));
    memcpy(preds, b->preds, sizeof(Block *) * npreds);
    bool local = false;
    for (int pi = 0; pi < npreds; pi++) {
      Block *p = preds[pi];
      if (p == b || (has_phi && ir_pred_index(target, p) >= 0))
        continue;
      Inst *pt = p->last;
      for (int k = 0; k < pt->nblocks; k++)
        if (pt->blocks[k] == b)
          pt->blocks[k] = target;
      for (Inst *i = target->first; i && i->op == IR_PHI; i = i->next) {
        Inst *v = NULL;
        for (int k = 0; k < i->nargs; k++)
          if (i->blocks[k] == b)
            v = i->args[k];
        ASSERT(v);
        int n = i->nargs;
        Inst **args = xalloc(sizeof(Inst *) * (n + 1));
        Block **blocks = xalloc(sizeof(Block *) * (n + 1));
        memcpy(args, i->args, sizeof(Inst *) * n);
        memcpy(blocks, i->blocks, sizeof(Block *) * n);
        args[n] = v;
        blocks[n] = p;
        i->args = args;
        i->blocks = blocks;
        i->nargs = n + 1;
        i->nblocks = n + 1;
      }
      // keep target's predecessor list current for the check above
      ir_compute_preds(f);
      local = true;
    }
    if (local)
      changed = true;
  }
  return changed;
}

void opt_simplify_cfg(Func *f) {
  ir_compute_preds(f);
  for (int round = 0; round < 50; round++) {
    bool changed = false;
    changed |= fold_branches(f);
    changed |= remove_unreachable(f);
    changed |= thread_jumps(f);
    changed |= remove_unreachable(f);
    changed |= merge_blocks(f);
    if (!changed)
      break;
  }
  opt_remove_trivial_phis(f);
  ir_verify(f, "simplify-cfg");
}

// An edge from a block with several successors to a block with several
// predecessors has no place for the copies that implement phis; such
// edges get a block of their own.
void opt_split_critical_edges(Func *f) {
  ir_compute_preds(f);
  for (Block *b = f->entry; b; b = b->next) {
    int n = ir_nsucc(b);
    if (n < 2)
      continue;
    for (int k = 0; k < n; k++) {
      Block *s = b->last->blocks[k];
      if (s->npreds < 2 || !(s->first && s->first->op == IR_PHI))
        continue;
      Block *mid = ir_new_block(f);
      Inst *j = ir_new(f, IR_JMP, IR_VOID, 0);
      j->blocks = xalloc(sizeof(Block *));
      j->blocks[0] = s;
      j->nblocks = 1;
      ir_append(mid, j);
      // Every edge from b to s goes through the same new block.
      for (int k2 = k; k2 < n; k2++)
        if (b->last->blocks[k2] == s)
          b->last->blocks[k2] = mid;
      ir_phi_replace_pred(s, b, mid);
      ir_insert_block_after(f, f->last, mid);
    }
  }
  ir_compute_preds(f);
  ir_verify(f, "split-critical-edges");
}

//
// Dominators (Cooper, Harvey and Kennedy, "A Simple, Fast Dominance
// Algorithm") and dominance frontiers.
//

static int rpo_counter;

static void number_rpo(Block *b, Block **order) {
  // iterative post-order DFS
  typedef struct {
    Block *b;
    int next;
  } Frame;
  Frame *stack = xalloc(sizeof(Frame) * 4);
  int cap = 4, sp = 0;
  stack[sp].b = b;
  stack[sp].next = 0;
  sp++;
  b->mark = true;
  while (sp) {
    Frame *fr = &stack[sp - 1];
    if (fr->next < ir_nsucc(fr->b)) {
      Block *s = ir_succ(fr->b, fr->next++);
      if (!s->mark) {
        s->mark = true;
        if (sp == cap) {
          stack = xgrow(stack, sizeof(Frame) * cap, sizeof(Frame) * cap * 2);
          cap *= 2;
        }
        stack[sp].b = s;
        stack[sp].next = 0;
        sp++;
      }
      continue;
    }
    order[rpo_counter++] = fr->b;
    sp--;
  }
}

static Block *intersect(Block *a, Block *b) {
  while (a != b) {
    while (a->rpo > b->rpo)
      a = a->idom;
    while (b->rpo > a->rpo)
      b = b->idom;
  }
  return a;
}

static int dom_counter;

static void number_domtree(Block *b) {
  // iterative preorder over the dominator tree
  Vec stack = {0};
  vec_push(&stack, b);
  while (stack.len) {
    Block *x = stack.data[--stack.len];
    if (x->dom_pre >= 0) {
      // second visit: close the subtree
      x->dom_post = dom_counter - 1;
      continue;
    }
    x->dom_pre = dom_counter++;
    x->dom_post = -1;
    vec_push(&stack, x); // revisit after the children
    for (int i = x->nkids - 1; i >= 0; i--)
      vec_push(&stack, x->kids[i]);
  }
}

bool dominates(Block *a, Block *b) {
  return a->dom_pre <= b->dom_pre && b->dom_pre <= a->dom_post;
}

void opt_dominators(Func *f) {
  ir_compute_preds(f);
  int n = f->nblocks;
  Block **post = xalloc(sizeof(Block *) * (n + 1));
  for (Block *b = f->entry; b; b = b->next) {
    b->mark = false;
    b->idom = NULL;
    b->rpo = -1;
    b->nkids = 0;
    b->ndf = 0;
    b->dom_pre = -1;
  }
  rpo_counter = 0;
  number_rpo(f->entry, post);
  int count = rpo_counter;
  // post[] holds post-order; RPO numbers count from the entry.
  Block **rpo = xalloc(sizeof(Block *) * (count + 1));
  for (int i = 0; i < count; i++) {
    rpo[i] = post[count - 1 - i];
    rpo[i]->rpo = i;
  }
  f->entry->idom = f->entry;
  bool changed = true;
  while (changed) {
    changed = false;
    for (int i = 1; i < count; i++) {
      Block *b = rpo[i];
      Block *nd = NULL;
      for (int k = 0; k < b->npreds; k++) {
        Block *p = b->preds[k];
        if (!p->idom)
          continue;
        nd = nd ? intersect(p, nd) : p;
      }
      if (nd != b->idom) {
        b->idom = nd;
        changed = true;
      }
    }
  }
  // Children lists in RPO order (deterministic).
  for (int i = 1; i < count; i++) {
    Block *b = rpo[i];
    Block *d = b->idom;
    d->kids = xgrow(d->kids, sizeof(Block *) * d->nkids, sizeof(Block *) * (d->nkids + 1));
    d->kids[d->nkids++] = b;
  }
  dom_counter = 0;
  number_domtree(f->entry);

  // Dominance frontiers.
  for (int i = 0; i < count; i++) {
    Block *b = rpo[i];
    if (b->npreds < 2)
      continue;
    for (int k = 0; k < b->npreds; k++) {
      Block *runner = b->preds[k];
      if (!runner->idom)
        continue;
      while (runner != b->idom) {
        bool present = false;
        for (int j = 0; j < runner->ndf; j++)
          if (runner->df[j] == b)
            present = true;
        if (!present) {
          runner->df = xgrow(runner->df, sizeof(Block *) * runner->ndf,
                             sizeof(Block *) * (runner->ndf + 1));
          runner->df[runner->ndf++] = b;
        }
        runner = runner->idom;
      }
    }
  }
}

// Loop nesting depth, from back edges (an edge to a dominator): every
// block from which the back edge's source is reachable without passing
// the header is in the loop.  Used to weight spill costs.
void opt_loop_depth(Func *f) {
  opt_dominators(f);
  for (Block *b = f->entry; b; b = b->next)
    b->loop_depth = 0;
  for (Block *b = f->entry; b; b = b->next) {
    for (int k = 0; k < ir_nsucc(b); k++) {
      Block *h = ir_succ(b, k);
      if (!dominates(h, b))
        continue;
      // natural loop of back edge b -> h
      for (Block *x = f->entry; x; x = x->next)
        x->mark = false;
      Vec stack = {0};
      h->mark = true;
      h->loop_depth++;
      if (!b->mark) {
        b->mark = true;
        b->loop_depth++;
        vec_push(&stack, b);
      }
      while (stack.len) {
        Block *x = stack.data[--stack.len];
        for (int p = 0; p < x->npreds; p++) {
          Block *y = x->preds[p];
          if (!y->mark) {
            y->mark = true;
            y->loop_depth++;
            vec_push(&stack, y);
          }
        }
      }
    }
  }
}
