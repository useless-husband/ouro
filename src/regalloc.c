// Register allocation: liveness analysis and linear scan (Poletto and
// Sarkar), then frame layout.
//
// Each virtual register gets one interval, the hull of every position
// where it is live; liveness comes from a standard backward data-flow
// analysis over the blocks, so values live around loop back edges are
// covered.  Intervals are visited by start position.  An interval that
// spans a call may only use a callee-saved register (x19-x28, d8-d15);
// others prefer caller-saved ones.  When no register is free the
// interval that ends last is spilled: it lives in a stack slot for its
// whole life and the emitter reloads it into a scratch register (x15,
// x16, x17, v29-v31) at each use.
//
// Physical registers appear in the machine code only inside call and
// return sequences and at function entry, all of which are parallel
// moves immediately next to the call or return; no virtual register is
// live in a caller-saved register across them (it would be live across
// the call).  x18 is never touched (Apple reserves it).

#include "ouro.h"

static Func *fn;

static void push_reg(int *arr, int *n, int r) {
  arr[(*n)++] = r;
}

// The registers an instruction reads and writes (virtual and physical).
int minst_regs(MInst *mi, int *uses, int *nuses, int *defs, int *ndefs) {
  *nuses = 0;
  *ndefs = 0;
  switch (mi->op) {
  case M_LABEL:
  case M_B:
  case M_BCOND:
  case M_TRAP:
  case M_RET:
  case M_BL:
    break;
  case M_MOVI:
  case M_FMOVI:
  case M_ADDR:
  case M_ARGADDR:
  case M_FRAME:
  case M_CSET:
    push_reg(defs, ndefs, mi->rd);
    break;
  case M_MOV:
  case M_FMOV:
  case M_FMOV_GF:
  case M_FMOV_FG:
  case M_NEG:
  case M_MVN:
  case M_SXTB:
  case M_SXTH:
  case M_SXTW:
  case M_UXTB:
  case M_UXTH:
  case M_UXTW:
  case M_FNEG:
  case M_SCVTF:
  case M_UCVTF:
  case M_FCVTZS:
  case M_FCVTZU:
  case M_FCVT:
  case M_ADDI:
  case M_ANDI:
  case M_ORRI:
  case M_EORI:
  case M_LSLI:
  case M_LSRI:
  case M_ASRI:
    push_reg(uses, nuses, mi->rn);
    push_reg(defs, ndefs, mi->rd);
    break;
  case M_ADD:
  case M_SUB:
  case M_AND:
  case M_ORR:
  case M_EOR:
  case M_MUL:
  case M_SDIV:
  case M_UDIV:
  case M_LSL:
  case M_LSR:
  case M_ASR:
  case M_FADD:
  case M_FSUB:
  case M_FMUL:
  case M_FDIV:
    push_reg(uses, nuses, mi->rn);
    push_reg(uses, nuses, mi->rm);
    push_reg(defs, ndefs, mi->rd);
    break;
  case M_MADD:
  case M_MSUB:
    push_reg(uses, nuses, mi->rn);
    push_reg(uses, nuses, mi->rm);
    push_reg(uses, nuses, mi->ra);
    push_reg(defs, ndefs, mi->rd);
    break;
  case M_CMP:
  case M_FCMP:
    push_reg(uses, nuses, mi->rn);
    push_reg(uses, nuses, mi->rm);
    break;
  case M_CMPI:
  case M_FCMPZ:
  case M_CBZ:
  case M_CBNZ:
  case M_JTAB:
  case M_BLR:
    push_reg(uses, nuses, mi->rn);
    break;
  case M_LDR:
    if (mi->mem.base != R_NONE)
      push_reg(uses, nuses, mi->mem.base);
    push_reg(defs, ndefs, mi->rd);
    break;
  case M_STR:
    push_reg(uses, nuses, mi->rn);
    if (mi->mem.base != R_NONE)
      push_reg(uses, nuses, mi->mem.base);
    break;
  case M_MEMCPY:
    if (mi->mem.base != R_NONE)
      push_reg(uses, nuses, mi->mem.base);
    if (mi->mem2.base != R_NONE)
      push_reg(uses, nuses, mi->mem2.base);
    break;
  case M_MEMZERO:
    if (mi->mem.base != R_NONE)
      push_reg(uses, nuses, mi->mem.base);
    break;
  case M_PMOVE:
    for (int k = 0; k < mi->pm->n; k++) {
      if (mi->pm->src[k] >= 0)
        push_reg(uses, nuses, mi->pm->src[k]);
      push_reg(defs, ndefs, mi->pm->dst[k]);
    }
    break;
  default:
    ICE("minst_regs: unknown op %d", mi->op);
  }
  return *nuses + *ndefs;
}

//
// Liveness
//

static int nwords;

static uint64_t *bits_new(void) {
  return xalloc(sizeof(uint64_t) * (nwords + 1));
}

static void bit_set(uint64_t *b, int v) {
  b[v >> 6] |= 1ULL << (v & 63);
}

static bool bit_test(uint64_t *b, int v) {
  return (b[v >> 6] >> (v & 63)) & 1;
}

// A live interval: the sorted, disjoint ranges [start, end) of
// positions where a virtual register holds a value it will need.
typedef struct {
  int start;
  int end;
} Range;

typedef struct {
  int vreg;
  int cls;
  Range *r;      // ranges in increasing order
  int nr;
  int cap;
  int cur;       // scan cursor into r
  int reg;       // assigned physical register, or -1
  int weight;    // spill cost: uses and definitions, weighted by loop depth
  bool cross;    // live across a call
} Interval;

static int max_operands(MInst *mi) {
  if (mi->op == M_PMOVE)
    return mi->pm->n * 2 + 2;
  return 8;
}

static void compute_liveness(Block **blocks, int nblocks, uint64_t **gen, uint64_t **kill) {
  int *uses = xalloc(sizeof(int) * 8);
  int *defs = xalloc(sizeof(int) * 8);
  int cap = 8;
  for (int bi = 0; bi < nblocks; bi++) {
    Block *b = blocks[bi];
    gen[bi] = bits_new();
    kill[bi] = bits_new();
    b->live_in = bits_new();
    b->live_out = bits_new();
    for (MInst *mi = b->mfirst; mi; mi = mi->next) {
      int n = max_operands(mi);
      if (n > cap) {
        uses = xalloc(sizeof(int) * n);
        defs = xalloc(sizeof(int) * n);
        cap = n;
      }
      int nu, nd;
      minst_regs(mi, uses, &nu, defs, &nd);
      for (int k = 0; k < nu; k++)
        if (is_vreg(uses[k]) && !bit_test(kill[bi], uses[k] - R_VREG))
          bit_set(gen[bi], uses[k] - R_VREG);
      for (int k = 0; k < nd; k++)
        if (is_vreg(defs[k]))
          bit_set(kill[bi], defs[k] - R_VREG);
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (int bi = nblocks - 1; bi >= 0; bi--) {
      Block *b = blocks[bi];
      for (int k = 0; k < ir_nsucc(b); k++) {
        Block *s = ir_succ(b, k);
        for (int w = 0; w < nwords; w++)
          b->live_out[w] |= s->live_in[w];
      }
      for (int w = 0; w < nwords; w++) {
        uint64_t in = gen[bi][w] | (b->live_out[w] & ~kill[bi][w]);
        if (in != b->live_in[w]) {
          b->live_in[w] = in;
          changed = true;
        }
      }
    }
  }
}

// Ranges are added from the end of the function backwards, so a new
// range starts no later than the first one stored: they are collected
// in reverse and flipped once all blocks are done.
static void add_range(Interval *it, int from, int to) {
  if (it->nr > 0) {
    Range *last = &it->r[it->nr - 1];
    if (to >= last->start) {
      if (from < last->start)
        last->start = from;
      if (to > last->end)
        last->end = to;
      return;
    }
  }
  if (it->nr == it->cap) {
    int cap = it->cap ? it->cap * 2 : 4;
    it->r = xgrow(it->r, sizeof(Range) * it->cap, sizeof(Range) * cap);
    it->cap = cap;
  }
  it->r[it->nr].start = from;
  it->r[it->nr].end = to;
  it->nr++;
}

// A definition starts the value's life: shorten the range that was
// opened at the beginning of the block.
static void set_from(Interval *it, int from) {
  if (it->nr == 0) {
    add_range(it, from, from + 1); // a definition that is never used
    return;
  }
  it->r[it->nr - 1].start = from;
}

static void finish_ranges(Interval *it) {
  for (int i = 0, j = it->nr - 1; i < j; i++, j--) {
    Range t = it->r[i];
    it->r[i] = it->r[j];
    it->r[j] = t;
  }
}

static int iv_start(Interval *it) {
  return it->r[0].start;
}

static int iv_end(Interval *it) {
  return it->r[it->nr - 1].end;
}

// Whether position p is inside the interval; the cursor only moves
// forward because the scan visits positions in increasing order.
static bool covers(Interval *it, int p) {
  while (it->cur < it->nr && it->r[it->cur].end <= p)
    it->cur++;
  return it->cur < it->nr && it->r[it->cur].start <= p;
}

// The first position where both intervals are live, or INT32_MAX.
static int first_intersection(Interval *a, Interval *b) {
  int i = 0, j = 0;
  while (i < a->nr && j < b->nr) {
    Range *x = &a->r[i], *y = &b->r[j];
    int lo = x->start > y->start ? x->start : y->start;
    int hi = x->end < y->end ? x->end : y->end;
    if (lo < hi)
      return lo;
    if (x->end <= y->end)
      i++;
    else
      j++;
  }
  return INT32_MAX;
}

//
// Linear scan
//

static const int gpr_caller[] = {9, 10, 11, 12, 13, 14, 8, 7, 6, 5, 4, 3, 2, 1, 0};
static const int gpr_callee[] = {19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
static const int fpr_caller[] = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                                 7,  6,  5,  4,  3,  2,  1,  0};
static const int fpr_callee[] = {8, 9, 10, 11, 12, 13, 14, 15};

static int *call_pos;
static int ncalls;

// A call clobbers the caller-saved registers; a value live across one
// (inside a range, not in a hole) needs a callee-saved register.
static bool crosses_call(Interval *it) {
  for (int k = 0; k < it->nr; k++) {
    int lo = 0, hi = ncalls;
    while (lo < hi) {
      int mid = (lo + hi) / 2;
      if (call_pos[mid] <= it->r[k].start)
        lo = mid + 1;
      else
        hi = mid;
    }
    // a call at position c clobbers values live from before c to after
    // c + 1 (the call's results are defined at c + 2 by the next move)
    if (lo < ncalls && call_pos[lo] + 1 < it->r[k].end)
      return true;
  }
  return false;
}

static int cmp_start(const void *a, const void *b) {
  const Interval *x = *(const Interval **)a;
  const Interval *y = *(const Interval **)b;
  int sx = iv_start((Interval *)x), sy = iv_start((Interval *)y);
  if (sx != sy)
    return sx < sy ? -1 : 1;
  return x->vreg < y->vreg ? -1 : x->vreg > y->vreg;
}

static int new_spill_slot(void) {
  int idx = fn->nslots++;
  fn->slot_size = xgrow(fn->slot_size, sizeof(int) * idx, sizeof(int) * (idx + 1));
  fn->slot_align = xgrow(fn->slot_align, sizeof(int) * idx, sizeof(int) * (idx + 1));
  fn->slot_size[idx] = 8;
  fn->slot_align[idx] = 8;
  return idx;
}

static void spill(Interval *it) {
  fn->vreg_reg[it->vreg - R_VREG] = -1;
  fn->vreg_slot[it->vreg - R_VREG] = new_spill_slot();
  it->reg = -1;
}

static bool reg_allowed(Interval *it, int reg) {
  int base = it->cls ? R_V0 : 0;
  if (reg < base || reg >= base + 32)
    return false;
  int r = reg - base;
  if (it->cls) {
    if (r > 28)
      return false; // v29-v31 are scratch
    return !it->cross || (r >= 8 && r <= 15);
  }
  if (!(r <= 14 || (r >= 19 && r <= 28)))
    return false; // x15-x18 are scratch / reserved, x29-x31 special
  return !it->cross || (r >= 19 && r <= 28);
}

static void take_reg(Interval *it, int reg) {
  it->reg = reg;
  fn->vreg_reg[it->vreg - R_VREG] = reg;
  int base = it->cls ? R_V0 : 0;
  int r = reg - base;
  if (it->cls == 0 && r >= 19 && r <= 28)
    fn->used_callee_gpr |= 1u << (r - 19);
  if (it->cls == 1 && r >= 8 && r <= 15)
    fn->used_callee_fpr |= 1u << (r - 8);
}

typedef struct {
  Interval **v;
  int n;
} IvList;

static void list_remove(IvList *l, int k) {
  l->v[k] = l->v[--l->n];
}

static void linear_scan(Interval **sorted, int n) {
  IvList active = {xalloc(sizeof(Interval *) * (n + 1)), 0};
  IvList inactive = {xalloc(sizeof(Interval *) * (n + 1)), 0};
  int free_until[96];

  for (int i = 0; i < n; i++) {
    Interval *cur = sorted[i];
    int pos = iv_start(cur);

    // Retire or park the intervals that no longer cover this position.
    for (int k = 0; k < active.n;) {
      Interval *a = active.v[k];
      if (iv_end(a) <= pos) {
        list_remove(&active, k);
      } else if (!covers(a, pos)) {
        inactive.v[inactive.n++] = a;
        list_remove(&active, k);
      } else {
        k++;
      }
    }
    for (int k = 0; k < inactive.n;) {
      Interval *a = inactive.v[k];
      if (iv_end(a) <= pos) {
        list_remove(&inactive, k);
      } else if (covers(a, pos)) {
        active.v[active.n++] = a;
        list_remove(&inactive, k);
      } else {
        k++;
      }
    }

    // How long each register stays free.
    for (int r = 0; r < 96; r++)
      free_until[r] = INT32_MAX;
    for (int k = 0; k < active.n; k++)
      if (active.v[k]->reg >= 0)
        free_until[active.v[k]->reg] = 0;
    for (int k = 0; k < inactive.n; k++) {
      Interval *a = inactive.v[k];
      if (a->reg < 0 || a->cls != cur->cls || free_until[a->reg] == 0)
        continue;
      int x = first_intersection(a, cur);
      if (x < free_until[a->reg])
        free_until[a->reg] = x;
    }

    int end = iv_end(cur);
    int reg = -1;
    int base = cur->cls ? R_V0 : 0;
    int h = fn->vreg_hint[cur->vreg - R_VREG];
    if (h >= 0 && is_vreg(h))
      h = fn->vreg_reg[h - R_VREG]; // hinted to another virtual register
    if (h >= 0 && reg_allowed(cur, h) && free_until[h] >= end)
      reg = h;
    if (reg < 0) {
      const int *pools[2];
      int sizes[2];
      if (cur->cls) {
        pools[0] = fpr_caller;
        sizes[0] = sizeof(fpr_caller) / sizeof(int);
        pools[1] = fpr_callee;
        sizes[1] = 8;
      } else {
        pools[0] = gpr_caller;
        sizes[0] = sizeof(gpr_caller) / sizeof(int);
        pools[1] = gpr_callee;
        sizes[1] = 10;
      }
      for (int p = 0; p < 2 && reg < 0; p++)
        for (int k = 0; k < sizes[p] && reg < 0; k++) {
          int r = base + pools[p][k];
          if (reg_allowed(cur, r) && free_until[r] >= end)
            reg = r;
        }
    }

    if (reg < 0) {
      // No register is free for the whole interval.  Find the register
      // whose current occupants (active, or inactive and overlapping)
      // are cheapest to spill; if they cost more than this interval,
      // spill this one instead.
      int best = -1;
      long best_cost = 0;
      for (int r = base; r < base + 32; r++) {
        if (!reg_allowed(cur, r))
          continue;
        long cost = 0;
        for (int k = 0; k < active.n; k++)
          if (active.v[k]->reg == r)
            cost += active.v[k]->weight;
        for (int k = 0; k < inactive.n; k++)
          if (inactive.v[k]->reg == r && first_intersection(inactive.v[k], cur) < INT32_MAX)
            cost += inactive.v[k]->weight;
        if (best < 0 || cost < best_cost) {
          best = r;
          best_cost = cost;
        }
      }
      if (best >= 0 && best_cost < cur->weight) {
        for (int k = 0; k < active.n;) {
          if (active.v[k]->reg == best) {
            spill(active.v[k]);
            list_remove(&active, k);
          } else {
            k++;
          }
        }
        for (int k = 0; k < inactive.n;) {
          Interval *a = inactive.v[k];
          if (a->reg == best && first_intersection(a, cur) < INT32_MAX) {
            spill(a);
            list_remove(&inactive, k);
          } else {
            k++;
          }
        }
        reg = best;
      }
    }

    if (reg >= 0) {
      take_reg(cur, reg);
      active.v[active.n++] = cur;
    } else {
      spill(cur);
    }
  }
}

//
// Frame layout
//
// High addresses
//   incoming stack arguments          x29 + 16 ...
//   saved x29, x30                    x29
//   saved callee-saved registers      x29 - 16 * k
//   stack slots (locals and spills)
//   outgoing stack arguments          sp
// Low addresses

static void layout_frame(void) {
  int64_t off = align_up(fn->out_bytes, 16);
  fn->slot_off = xalloc(sizeof(int64_t) * (fn->nslots + 1));
  // Spill slots first, nearest to sp: the reload code assumes their
  // offsets fit the scaled 12-bit immediate of ldr/str.
  for (int i = fn->nallocas; i < fn->nslots; i++) {
    off = align_up(off, 8);
    fn->slot_off[i] = off;
    off += 8;
  }
  if (off > 32760)
    fatal("function '%s' needs too many spill slots", fn->name);
  for (int i = 0; i < fn->nallocas; i++) {
    int a = fn->slot_align[i];
    if (a > 16)
      a = 16; // sp is only 16-byte aligned; larger alignments are not honoured
    off = align_up(off, a);
    fn->slot_off[i] = off;
    off += fn->slot_size[i];
  }
  fn->frame_size = align_up(off, 16);
  int nsaved = 0;
  for (int r = 0; r < 10; r++)
    if (fn->used_callee_gpr & (1u << r))
      nsaved++;
  int nfsaved = 0;
  for (int r = 0; r < 8; r++)
    if (fn->used_callee_fpr & (1u << r))
      nfsaved++;
  fn->callee_area = 16 * ((nsaved + 1) / 2) + 16 * ((nfsaved + 1) / 2);
}

void regalloc_func(Func *f) {
  fn = f;
  int nv = f->nvregs;
  f->vreg_reg = xalloc(sizeof(int) * (nv + 1));
  f->vreg_slot = xalloc(sizeof(int) * (nv + 1));
  for (int i = 0; i < nv; i++) {
    f->vreg_reg[i] = -1;
    f->vreg_slot[i] = -1;
  }
  f->used_callee_gpr = 0;
  f->used_callee_fpr = 0;
  nwords = (nv + 63) / 64 + 1;

  Block **blocks = xalloc(sizeof(Block *) * (f->nblocks + 1));
  int nblocks = 0;
  for (Block *b = f->entry; b; b = b->next)
    blocks[nblocks++] = b;

  // Positions: two per instruction (uses at 2i, definitions at 2i+1).
  int pos = 0;
  ncalls = 0;
  for (int bi = 0; bi < nblocks; bi++) {
    Block *b = blocks[bi];
    b->mpos_start = pos;
    for (MInst *mi = b->mfirst; mi; mi = mi->next) {
      mi->pos = pos;
      pos += 2;
      if (mi->op == M_BL || mi->op == M_BLR)
        ncalls++;
    }
    b->mpos_end = pos; // exclusive
  }
  call_pos = xalloc(sizeof(int) * (ncalls + 1));
  ncalls = 0;
  for (int bi = 0; bi < nblocks; bi++)
    for (MInst *mi = blocks[bi]->mfirst; mi; mi = mi->next)
      if (mi->op == M_BL || mi->op == M_BLR)
        call_pos[ncalls++] = mi->pos;

  uint64_t **gen = xalloc(sizeof(uint64_t *) * (nblocks + 1));
  uint64_t **kill = xalloc(sizeof(uint64_t *) * (nblocks + 1));
  compute_liveness(blocks, nblocks, gen, kill);
  // Nothing is live into the entry block: a virtual register that is
  // would be read before it is ever written (an instruction selector
  // bug, such as emitting a use before the code that computes it).
  for (int v = 0; v < nv; v++)
    if (bit_test(f->entry->live_in, v)) {
      if (getenv("OURO_VERIFY"))
        mir_dump(stderr, f);
      ICE("%s: virtual register %%%d is used before it is defined", f->name, v);
    }

  // Build live ranges, walking blocks and instructions backwards
  // (Wimmer and Moessenboeck, "Optimized Interval Splitting in a Linear
  // Scan Register Allocator", 2005, without the splitting).
  Interval *iv = xalloc(sizeof(Interval) * (nv + 1));
  for (int v = 0; v < nv; v++) {
    iv[v].vreg = R_VREG + v;
    iv[v].reg = -1;
    iv[v].cls = f->vreg_cls[v];
  }
  int *uses = xalloc(sizeof(int) * 8);
  int *defs = xalloc(sizeof(int) * 8);
  int cap = 8;
  MInst **order = xalloc(sizeof(MInst *) * (pos / 2 + 1));
  for (int bi = nblocks - 1; bi >= 0; bi--) {
    Block *b = blocks[bi];
    int depth = b->loop_depth < 3 ? b->loop_depth : 3;
    int w = depth == 0 ? 1 : depth == 1 ? 10 : depth == 2 ? 100 : 1000;
    for (int v = 0; v < nv; v++)
      if (bit_test(b->live_out, v))
        add_range(&iv[v], b->mpos_start, b->mpos_end);
    int ninsts = 0;
    for (MInst *mi = b->mfirst; mi; mi = mi->next)
      order[ninsts++] = mi;
    for (int k = ninsts - 1; k >= 0; k--) {
      MInst *mi = order[k];
      int n = max_operands(mi);
      if (n > cap) {
        uses = xalloc(sizeof(int) * n);
        defs = xalloc(sizeof(int) * n);
        cap = n;
      }
      int nu, nd;
      minst_regs(mi, uses, &nu, defs, &nd);
      for (int j = 0; j < nd; j++)
        if (is_vreg(defs[j])) {
          Interval *it = &iv[defs[j] - R_VREG];
          set_from(it, mi->pos + 1);
          it->weight += w;
        }
      for (int j = 0; j < nu; j++)
        if (is_vreg(uses[j])) {
          Interval *it = &iv[uses[j] - R_VREG];
          add_range(it, b->mpos_start, mi->pos + 1);
          it->weight += w;
        }
    }
  }

  Interval **sorted = xalloc(sizeof(Interval *) * (nv + 1));
  int n = 0;
  for (int v = 0; v < nv; v++) {
    if (iv[v].nr == 0)
      continue;
    finish_ranges(&iv[v]);
    iv[v].cross = crosses_call(&iv[v]);
    sorted[n++] = &iv[v];
  }
  qsort(sorted, n, sizeof(Interval *), cmp_start);
  linear_scan(sorted, n);
  layout_frame();
  if (getenv("OURO_RA_STATS")) {
    int spilled = 0, crossing = 0;
    for (int k = 0; k < n; k++) {
      crossing += sorted[k]->cross;
      spilled += sorted[k]->reg < 0;
    }
    fprintf(stderr, "%s: %d intervals, %d cross calls, %d spilled, %d positions, %d calls\n",
            f->name, n, crossing, spilled, pos, ncalls);
  }
}

//
// Debug dump (--dump-mir)
//

static const char *mop_name[] = {
    "label",  "mov",   "fmov",  "movi",  "fmovi", "fmov.gf", "fmov.fg", "addr",  "frame",
    "argaddr", "add",  "sub",   "and",   "orr",   "eor",     "addi",    "andi",  "orri",
    "eori",   "mul",   "madd",  "msub",  "sdiv",  "udiv",    "lsl",     "lsr",   "asr",
    "lsli",   "lsri",  "asri",  "neg",   "mvn",   "cmp",     "cmpi",    "fcmp",  "fcmpz",
    "cset",   "sxtb",  "sxth",  "sxtw",  "uxtb",  "uxth",    "uxtw",    "ldr",   "str",
    "fadd",   "fsub",  "fmul",  "fdiv",  "fneg",  "scvtf",   "ucvtf",   "fcvtzs", "fcvtzu",
    "fcvt",   "bl",    "blr",   "b",     "b.cond", "cbz",    "cbnz",    "ret",   "pmove",
    "jtab",   "memcpy", "memzero", "trap",
};

static void print_reg(FILE *out, Func *f, int r) {
  if (r == R_NONE) {
    fprintf(out, "-");
    return;
  }
  if (r == R_ZR) {
    fprintf(out, "zr");
    return;
  }
  if (r == R_SP) {
    fprintf(out, "sp");
    return;
  }
  if (r < R_V0) {
    fprintf(out, "x%d", r);
    return;
  }
  if (r < R_VREG) {
    fprintf(out, "v%d", r - R_V0);
    return;
  }
  fprintf(out, "%%%d", r - R_VREG);
  if (f->vreg_reg && f->vreg_reg[r - R_VREG] >= 0) {
    fprintf(out, "(");
    print_reg(out, f, f->vreg_reg[r - R_VREG]);
    fprintf(out, ")");
  } else if (f->vreg_slot && f->vreg_slot[r - R_VREG] >= 0) {
    fprintf(out, "(slot%d)", f->vreg_slot[r - R_VREG]);
  }
}

void mir_dump(FILE *out, Func *f) {
  fprintf(out, "machine function %s (frame %lld, callee-saved area %lld)\n", f->name,
          (long long)f->frame_size, (long long)f->callee_area);
  for (Block *b = f->entry; b; b = b->next) {
    for (MInst *mi = b->mfirst; mi; mi = mi->next) {
      if (mi->op == M_LABEL) {
        fprintf(out, "bb%d:\n", b->id);
        continue;
      }
      fprintf(out, "  %4d %s", mi->pos, mop_name[mi->op]);
      if (mi->op == M_PMOVE) {
        for (int k = 0; k < mi->pm->n; k++) {
          fprintf(out, k ? ", " : " ");
          print_reg(out, f, mi->pm->dst[k]);
          fprintf(out, "<-");
          if (mi->pm->src[k] >= 0)
            print_reg(out, f, mi->pm->src[k]);
          else
            fprintf(out, "#%lld", (long long)mi->pm->imm[k]);
        }
        fprintf(out, "\n");
        continue;
      }
      fprintf(out, " ");
      print_reg(out, f, mi->rd);
      fprintf(out, ", ");
      print_reg(out, f, mi->rn);
      fprintf(out, ", ");
      print_reg(out, f, mi->rm);
      if (mi->imm)
        fprintf(out, ", #%lld", (long long)mi->imm);
      if (mi->mem.base != R_NONE || mi->mem.slot >= 0) {
        fprintf(out, ", [");
        if (mi->mem.slot >= 0)
          fprintf(out, "slot%d", mi->mem.slot);
        else
          print_reg(out, f, mi->mem.base);
        fprintf(out, "%+lld]", (long long)mi->mem.off);
      }
      if (mi->target)
        fprintf(out, " -> bb%d", mi->target->id);
      fprintf(out, "\n");
    }
  }
}
