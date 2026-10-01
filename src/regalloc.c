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

typedef struct {
  int vreg;
  int start;
  int end;
  int reg;
  int cls;
  int spill_weight;
} Interval;

static int max_operands(MInst *mi) {
  if (mi->op == M_PMOVE)
    return mi->pm->n * 2 + 2;
  return 8;
}

static void compute_liveness(Block **blocks, int nblocks, uint64_t **gen, uint64_t **kill) {
  for (int bi = 0; bi < nblocks; bi++) {
    Block *b = blocks[bi];
    gen[bi] = bits_new();
    kill[bi] = bits_new();
    b->live_in = bits_new();
    b->live_out = bits_new();
    for (MInst *mi = b->mfirst; mi; mi = mi->next) {
      int n = max_operands(mi);
      int *uses = xalloc(sizeof(int) * n);
      int *defs = xalloc(sizeof(int) * n);
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

static bool crosses_call(Interval *it) {
  // binary search for a call strictly inside (start, end)
  int lo = 0, hi = ncalls;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (call_pos[mid] <= it->start)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < ncalls && call_pos[lo] < it->end;
}

static int cmp_start(const void *a, const void *b) {
  const Interval *x = *(const Interval **)a;
  const Interval *y = *(const Interval **)b;
  if (x->start != y->start)
    return x->start < y->start ? -1 : 1;
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

static void linear_scan(Interval **sorted, int n) {
  // `active` is kept sorted by end position.
  Interval **active = xalloc(sizeof(Interval *) * (n + 1));
  int nactive = 0;
  bool used[96];
  memset(used, 0, sizeof(used));

  for (int i = 0; i < n; i++) {
    Interval *it = sorted[i];
    // Expire intervals that ended before this one starts.
    int out = 0;
    for (int k = 0; k < nactive; k++) {
      if (active[k]->end < it->start) {
        if (active[k]->reg >= 0)
          used[active[k]->reg] = false;
      } else {
        active[out++] = active[k];
      }
    }
    nactive = out;

    bool cross = crosses_call(it);
    int base = it->cls ? R_V0 : 0;
    int reg = -1;

    // A hint (a register the value is moved from or to) wins if free.
    int h = fn->vreg_hint[it->vreg - R_VREG];
    if (h >= 0 && h >= base && h < base + 32 && !used[h]) {
      int r = h - base;
      bool callee = it->cls ? (r >= 8 && r <= 15) : (r >= 19 && r <= 28);
      bool ok = it->cls ? (r <= 28 && r != 29) : (r <= 14 || (r >= 19 && r <= 28));
      if (ok && (!cross || callee))
        reg = h;
    }
    if (reg < 0 && !cross) {
      const int *pool = it->cls ? fpr_caller : gpr_caller;
      int np = it->cls ? (int)(sizeof(fpr_caller) / sizeof(int)) : (int)(sizeof(gpr_caller) / sizeof(int));
      for (int k = 0; k < np && reg < 0; k++)
        if (!used[base + pool[k]])
          reg = base + pool[k];
    }
    if (reg < 0) {
      const int *pool = it->cls ? fpr_callee : gpr_callee;
      int np = it->cls ? 8 : 10;
      for (int k = 0; k < np && reg < 0; k++)
        if (!used[base + pool[k]])
          reg = base + pool[k];
    }

    if (reg < 0) {
      // Spill the active interval (of a usable register) that ends
      // last, or this one if it ends later still.
      Interval *victim = NULL;
      for (int k = 0; k < nactive; k++) {
        Interval *a = active[k];
        if (a->reg < 0 || a->cls != it->cls)
          continue;
        int r = a->reg - base;
        bool callee = it->cls ? (r >= 8 && r <= 15) : (r >= 19 && r <= 28);
        if (cross && !callee)
          continue;
        if (!victim || a->end > victim->end ||
            (a->end == victim->end && a->vreg > victim->vreg))
          victim = a;
      }
      if (victim && victim->end > it->end) {
        reg = victim->reg;
        spill(victim);
      } else {
        spill(it);
      }
    }

    if (reg >= 0) {
      it->reg = reg;
      used[reg] = true;
      fn->vreg_reg[it->vreg - R_VREG] = reg;
      int r = reg - base;
      if (it->cls == 0 && r >= 19 && r <= 28)
        fn->used_callee_gpr |= 1u << (r - 19);
      if (it->cls == 1 && r >= 8 && r <= 15)
        fn->used_callee_fpr |= 1u << (r - 8);
    }
    // insert into active, keeping it sorted by end
    int pos = nactive;
    while (pos > 0 && active[pos - 1]->end > it->end)
      pos--;
    for (int k = nactive; k > pos; k--)
      active[k] = active[k - 1];
    active[pos] = it;
    nactive++;
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
    b->mpos_end = pos - 1;
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

  Interval *iv = xalloc(sizeof(Interval) * (nv + 1));
  for (int v = 0; v < nv; v++) {
    iv[v].vreg = R_VREG + v;
    iv[v].start = INT32_MAX;
    iv[v].end = -1;
    iv[v].reg = -1;
    iv[v].cls = f->vreg_cls[v];
  }
#define EXTEND(v, p)                                                                               \
  do {                                                                                             \
    Interval *x_ = &iv[(v)-R_VREG];                                                                \
    if ((p) < x_->start)                                                                           \
      x_->start = (p);                                                                             \
    if ((p) > x_->end)                                                                             \
      x_->end = (p);                                                                               \
  } while (0)

  for (int bi = 0; bi < nblocks; bi++) {
    Block *b = blocks[bi];
    for (int v = 0; v < nv; v++) {
      if (bit_test(b->live_in, v))
        EXTEND(R_VREG + v, b->mpos_start);
      if (bit_test(b->live_out, v))
        EXTEND(R_VREG + v, b->mpos_end);
    }
    for (MInst *mi = b->mfirst; mi; mi = mi->next) {
      int n = max_operands(mi);
      int *uses = xalloc(sizeof(int) * n);
      int *defs = xalloc(sizeof(int) * n);
      int nu, nd;
      minst_regs(mi, uses, &nu, defs, &nd);
      for (int k = 0; k < nu; k++)
        if (is_vreg(uses[k])) {
          EXTEND(uses[k], mi->pos);
          iv[uses[k] - R_VREG].spill_weight += 1 + 9 * (b->loop_depth > 0);
        }
      for (int k = 0; k < nd; k++)
        if (is_vreg(defs[k])) {
          EXTEND(defs[k], mi->pos + 1);
          iv[defs[k] - R_VREG].spill_weight += 1 + 9 * (b->loop_depth > 0);
        }
    }
  }
#undef EXTEND

  Interval **sorted = xalloc(sizeof(Interval *) * (nv + 1));
  int n = 0;
  for (int v = 0; v < nv; v++)
    if (iv[v].end >= 0)
      sorted[n++] = &iv[v];
  qsort(sorted, n, sizeof(Interval *), cmp_start);
  linear_scan(sorted, n);
  layout_frame();
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
