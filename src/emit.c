// Assembly output for the system assembler (Apple's arm64 syntax).
//
// Register-allocated machine instructions are printed one by one.
// Spilled operands are reloaded into scratch registers that the
// allocator never hands out (x15-x17 and v29-v31); parallel moves are
// turned into a sequence of moves, breaking cycles through a scratch
// register; large immediates and offsets are built in scratch registers.

#include "ouro.h"

static FILE *out;
static Func *fn;
static bool needs_frame;

static void emitf(const char *f, ...) FMT(1, 2);

static void emitf(const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  vfprintf(out, f, ap);
  va_end(ap);
}

//
// Register names
//

static char *xname(int r, int sz) {
  static char bufs[8][8];
  static int n;
  char *b = bufs[n++ & 7];
  if (r == R_ZR)
    return sz == 8 ? "xzr" : "wzr";
  if (r == R_SP)
    return sz == 8 ? "sp" : "wsp";
  if (r == R_FP)
    return "x29";
  if (r == R_LR)
    return "x30";
  ASSERT(r >= 0 && r < 31);
  snprintf(b, 8, "%c%d", sz == 8 ? 'x' : 'w', r);
  return b;
}

static char *vname(int r, int sz) {
  static char bufs[8][8];
  static int n;
  char *b = bufs[n++ & 7];
  ASSERT(r >= R_V0 && r < R_V0 + 32);
  snprintf(b, 8, "%c%d", sz == 8 ? 'd' : 's', r - R_V0);
  return b;
}

static char *rname(int r, int sz) {
  if (r >= R_V0 && r < R_VREG)
    return vname(r, sz);
  return xname(r, sz);
}

//
// Immediates
//

// Materialises an arbitrary 64-bit (or 32-bit) constant in `rd`.
static void emit_mov_imm(int rd, int64_t v, int sz) {
  if (sz == 4)
    v = (int64_t)(uint32_t)v;
  uint64_t u = (uint64_t)v;
  if (u == 0) {
    emitf("\tmov %s, #0\n", xname(rd, sz));
    return;
  }
  // A single movz or movn.
  int nchunks = sz == 8 ? 4 : 2;
  uint64_t inv = sz == 8 ? ~u : (~u & 0xFFFFFFFFULL);
  int nonzero = 0, nonffff = 0;
  for (int i = 0; i < nchunks; i++) {
    if ((u >> (16 * i)) & 0xFFFF)
      nonzero++;
    if ((inv >> (16 * i)) & 0xFFFF)
      nonffff++;
  }
  uint32_t enc;
  if (nonzero > 1 && nonffff > 1 && encode_logical_imm(u, sz, &enc)) {
    emitf("\torr %s, %s, #0x%llx\n", xname(rd, sz), xname(R_ZR, sz), (unsigned long long)u);
    return;
  }
  if (nonffff < nonzero) {
    // movn then movk for the chunks that are not 0xffff
    bool first = true;
    for (int i = 0; i < nchunks; i++) {
      uint64_t chunk = (u >> (16 * i)) & 0xFFFF;
      if (first) {
        if (chunk == 0xFFFF && i < nchunks - 1 && nonffff > 0)
          continue;
        emitf("\tmovn %s, #0x%llx, lsl #%d\n", xname(rd, sz),
              (unsigned long long)((~chunk) & 0xFFFF), 16 * i);
        first = false;
      } else if (chunk != 0xFFFF) {
        emitf("\tmovk %s, #0x%llx, lsl #%d\n", xname(rd, sz), (unsigned long long)chunk, 16 * i);
      }
    }
    if (first)
      emitf("\tmovn %s, #0\n", xname(rd, sz));
    return;
  }
  if (nonzero == 1 && (u & 0xFFFF) == u) {
    emitf("\tmov %s, #%llu\n", xname(rd, sz), (unsigned long long)u);
    return;
  }
  bool first = true;
  for (int i = 0; i < nchunks; i++) {
    uint64_t chunk = (u >> (16 * i)) & 0xFFFF;
    if (chunk == 0)
      continue;
    emitf("\t%s %s, #0x%llx, lsl #%d\n", first ? "movz" : "movk", xname(rd, sz),
          (unsigned long long)chunk, 16 * i);
    first = false;
  }
}

// rd = rn + imm for any imm (rn may be sp).  Uses x17 for immediates
// that do not fit two add/sub instructions.
static void emit_add_imm(int rd, int rn, int64_t imm, int sz) {
  const char *op = imm < 0 ? "sub" : "add";
  uint64_t a = imm < 0 ? (uint64_t)(-imm) : (uint64_t)imm;
  if (a == 0) {
    if (rd != rn)
      emitf("\tmov %s, %s\n", xname(rd, sz), xname(rn, sz));
    return;
  }
  if (a < 4096) {
    emitf("\t%s %s, %s, #%llu\n", op, xname(rd, sz), xname(rn, sz), (unsigned long long)a);
    return;
  }
  if (a < (1ULL << 24)) {
    emitf("\t%s %s, %s, #%llu, lsl #12\n", op, xname(rd, sz), xname(rn, sz),
          (unsigned long long)(a >> 12));
    if (a & 0xFFF)
      emitf("\t%s %s, %s, #%llu\n", op, xname(rd, sz), xname(rd, sz),
            (unsigned long long)(a & 0xFFF));
    return;
  }
  ASSERT(rd != R_X17 || rn != R_X17 || true);
  emit_mov_imm(R_X17, (int64_t)a, 8);
  emitf("\t%s %s, %s, x17\n", op, xname(rd, sz), xname(rn, sz));
}

bool fmov_imm_ok(double v, int sz) {
  // fmov accepts +/- n/16 * 2^e with n in 16..31 and e in -3..4.
  (void)sz;
  if (v == 0)
    return false;
  double a = v < 0 ? -v : v;
  for (int e = -3; e <= 4; e++)
    for (int n = 16; n <= 31; n++) {
      double x = (double)n / 16.0;
      for (int k = 0; k < (e > 0 ? e : -e); k++)
        x = e > 0 ? x * 2 : x / 2;
      if (x == a)
        return true;
    }
  return false;
}

//
// Operands
//

static int vreg_phys(int r) {
  if (!is_vreg(r))
    return r;
  return fn->vreg_reg[r - R_VREG];
}

static int64_t slot_offset(int slot) {
  return fn->slot_off[slot];
}

static int64_t spill_offset(int r) {
  return slot_offset(fn->vreg_slot[r - R_VREG]);
}

static bool is_fpr_reg(int r) {
  return r >= R_V0 && r < R_VREG;
}

static int reg_cls(int r) {
  if (is_vreg(r))
    return fn->vreg_cls[r - R_VREG];
  return is_fpr_reg(r) ? 1 : 0;
}

static const int gpr_scratch[3] = {R_X16, R_X17, R_X15};
static const int fpr_scratch[3] = {R_V31, R_V30, R_V29};

// Returns the physical register holding `r`, reloading a spilled value
// into scratch register number `which` of its class.
static int use(int r, int which) {
  if (r == R_NONE || r == R_ZR || r == R_SP)
    return r;
  int p = vreg_phys(r);
  if (p >= 0)
    return p;
  int cls = reg_cls(r);
  int s = cls ? fpr_scratch[which] : gpr_scratch[which];
  emitf("\tldr %s, [sp, #%lld]\n", cls ? vname(s, 8) : xname(s, 8), (long long)spill_offset(r));
  return s;
}

static int pending_def = R_NONE;
static int pending_scratch;

// The physical register an instruction should write for `r`; a spilled
// value is written to a scratch register and stored by finish_def.
static int def(int r) {
  if (r == R_NONE)
    return r;
  int p = vreg_phys(r);
  if (p >= 0)
    return p;
  pending_def = r;
  pending_scratch = reg_cls(r) ? R_V31 : R_X16;
  return pending_scratch;
}

static void finish_def(void) {
  if (pending_def == R_NONE)
    return;
  int s = pending_scratch;
  emitf("\tstr %s, [sp, #%lld]\n", is_fpr_reg(s) ? vname(s, 8) : xname(s, 8),
        (long long)spill_offset(pending_def));
  pending_def = R_NONE;
}

static int access_size(int memty) {
  return ir_size(memty);
}

// Formats a memory operand.  An offset that does not fit the scaled
// 12-bit or unscaled 9-bit forms is added into `scratch` first.
// *unscaled is set when ldur/stur must be used.
static char *mem_operand(MemRef m, int size, int scratch, int base_which, bool *unscaled) {
  static char buf[64];
  *unscaled = false;
  int base;
  int64_t off = m.off;
  if (m.slot >= 0) {
    base = R_SP;
    off += slot_offset(m.slot);
  } else {
    base = use(m.base, base_which);
  }
  if (m.index != R_NONE) {
    // Register offset: [base, index{, extend #shift}].  The slot base
    // or a constant offset is first added into the scratch register.
    ASSERT(m.off == 0);
    int idx = use(m.index, 2);
    if (off != 0 || base == R_SP) {
      emit_add_imm(scratch, base, off, 8);
      base = scratch;
    }
    const char *ext = m.ext == 1 ? "sxtw" : m.ext == 2 ? "uxtw" : "lsl";
    if (m.shift || m.ext)
      snprintf(buf, sizeof(buf), "[%s, %s, %s #%d]", xname(base, 8), xname(idx, m.ext ? 4 : 8),
               ext, m.shift);
    else
      snprintf(buf, sizeof(buf), "[%s, %s]", xname(base, 8), xname(idx, 8));
    return buf;
  }
  if (off >= 0 && off % size == 0 && off / size < 4096) {
    if (off)
      snprintf(buf, sizeof(buf), "[%s, #%lld]", xname(base, 8), (long long)off);
    else
      snprintf(buf, sizeof(buf), "[%s]", xname(base, 8));
    return buf;
  }
  if (off >= -256 && off <= 255) {
    *unscaled = true;
    snprintf(buf, sizeof(buf), "[%s, #%lld]", xname(base, 8), (long long)off);
    return buf;
  }
  emit_add_imm(scratch, base, off, 8);
  snprintf(buf, sizeof(buf), "[%s]", xname(scratch, 8));
  return buf;
}

static const char *ldr_mnemonic(int memty, bool is_signed, bool unscaled) {
  switch (memty) {
  case IR_I8:
    return is_signed ? (unscaled ? "ldursb" : "ldrsb") : (unscaled ? "ldurb" : "ldrb");
  case IR_I16:
    return is_signed ? (unscaled ? "ldursh" : "ldrsh") : (unscaled ? "ldurh" : "ldrh");
  default:
    return unscaled ? "ldur" : "ldr";
  }
}

static const char *str_mnemonic(int memty, bool unscaled) {
  switch (memty) {
  case IR_I8:
    return unscaled ? "sturb" : "strb";
  case IR_I16:
    return unscaled ? "sturh" : "strh";
  default:
    return unscaled ? "stur" : "str";
  }
}

// The name of the register for a value of memory type memty.
static char *mem_reg(int r, int memty) {
  switch (memty) {
  case IR_F32:
    return vname(r, 4);
  case IR_F64:
    return vname(r, 8);
  case IR_I64:
    return xname(r, 8);
  default:
    return xname(r, 4);
  }
}

static const char *cond_name[] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
                                  "hi", "ls", "ge", "lt", "gt", "le"};

static int invert_cond(int c) {
  return c ^ 1; // conditions come in complementary pairs
}

//
// Labels
//

static char *block_label(Block *b) {
  return fmt("LBB%d_%d", fn->id, b->label);
}

static bool is_next_block(Block *cur, Block *target) {
  return cur->next == target;
}

//
// Parallel moves
//

enum { LOC_SLOT_BASE = 1000 };

static int loc_of(int r) {
  if (!is_vreg(r))
    return r;
  int p = vreg_phys(r);
  if (p >= 0)
    return p;
  return LOC_SLOT_BASE + fn->vreg_slot[r - R_VREG];
}

static void move_loc(int dst, int src, bool is_imm, int64_t imm, int sz) {
  if (!is_imm && dst == src)
    return;
  bool dslot = dst >= LOC_SLOT_BASE;
  int64_t doff = dslot ? slot_offset(dst - LOC_SLOT_BASE) : 0;
  if (is_imm) {
    if (dslot) {
      if (imm == 0) {
        emitf("\tstr xzr, [sp, #%lld]\n", (long long)doff);
      } else {
        emit_mov_imm(R_X15, imm, 8);
        emitf("\tstr x15, [sp, #%lld]\n", (long long)doff);
      }
    } else if (is_fpr_reg(dst)) {
      emit_mov_imm(R_X15, imm, 8);
      emitf("\tfmov %s, x15\n", vname(dst, 8));
    } else {
      emit_mov_imm(dst, imm, sz);
    }
    return;
  }
  bool sslot = src >= LOC_SLOT_BASE;
  int64_t soff = sslot ? slot_offset(src - LOC_SLOT_BASE) : 0;
  if (dslot && sslot) {
    emitf("\tldr x15, [sp, #%lld]\n\tstr x15, [sp, #%lld]\n", (long long)soff, (long long)doff);
    return;
  }
  if (dslot) {
    emitf("\tstr %s, [sp, #%lld]\n", rname(src, 8), (long long)doff);
    return;
  }
  if (sslot) {
    emitf("\tldr %s, [sp, #%lld]\n", rname(dst, 8), (long long)soff);
    return;
  }
  bool df = is_fpr_reg(dst), sf = is_fpr_reg(src);
  if (df && sf)
    emitf("\tfmov %s, %s\n", vname(dst, 8), vname(src, 8));
  else if (df)
    emitf("\tfmov %s, %s\n", vname(dst, 8), xname(src, 8));
  else if (sf)
    emitf("\tfmov %s, %s\n", xname(dst, 8), vname(src, 8));
  else
    emitf("\tmov %s, %s\n", xname(dst, 8), xname(src, 8));
}

// Sequentialises a parallel move: emit every move whose destination is
// no longer needed as a source; when only cycles remain, park one
// destination's value in a scratch register and redirect its readers.
static void emit_pmove(PMove *pm) {
  int n = pm->n;
  int *dst = xalloc(sizeof(int) * (n + 1));
  int *src = xalloc(sizeof(int) * (n + 1));
  bool *done = xalloc(sizeof(bool) * (n + 1));
  for (int k = 0; k < n; k++) {
    dst[k] = loc_of(pm->dst[k]);
    src[k] = pm->src[k] >= 0 ? loc_of(pm->src[k]) : -1;
    done[k] = pm->src[k] >= 0 && dst[k] == src[k];
  }
  for (;;) {
    bool progress = false;
    bool remaining = false;
    for (int k = 0; k < n; k++) {
      if (done[k])
        continue;
      remaining = true;
      bool blocked = false;
      for (int j = 0; j < n; j++)
        if (j != k && !done[j] && src[j] == dst[k])
          blocked = true;
      if (blocked)
        continue;
      move_loc(dst[k], src[k], src[k] < 0, pm->imm[k], pm->sz[k]);
      done[k] = true;
      progress = true;
    }
    if (!remaining)
      return;
    if (progress)
      continue;
    // Only cycles are left.
    int k = 0;
    while (done[k])
      k++;
    int d = dst[k];
    int tmp = is_fpr_reg(d) ? R_V31 : R_X16;
    move_loc(tmp, d, false, 0, 8);
    for (int j = 0; j < n; j++)
      if (!done[j] && src[j] == d)
        src[j] = tmp;
  }
}

//
// Prologue and epilogue
//

static void save_pairs(bool restore) {
  int regs[20];
  int n = 0;
  for (int r = 0; r < 10; r++)
    if (fn->used_callee_gpr & (1u << r))
      regs[n++] = 19 + r;
  int fregs[8];
  int nf = 0;
  for (int r = 0; r < 8; r++)
    if (fn->used_callee_fpr & (1u << r))
      fregs[nf++] = R_V0 + 8 + r;
  if (!restore) {
    for (int i = 0; i < n; i += 2) {
      if (i + 1 < n)
        emitf("\tstp %s, %s, [sp, #-16]!\n", xname(regs[i], 8), xname(regs[i + 1], 8));
      else
        emitf("\tstr %s, [sp, #-16]!\n", xname(regs[i], 8));
    }
    for (int i = 0; i < nf; i += 2) {
      if (i + 1 < nf)
        emitf("\tstp %s, %s, [sp, #-16]!\n", vname(fregs[i], 8), vname(fregs[i + 1], 8));
      else
        emitf("\tstr %s, [sp, #-16]!\n", vname(fregs[i], 8));
    }
    return;
  }
  for (int i = ((nf + 1) / 2 - 1) * 2; i >= 0; i -= 2) {
    if (i + 1 < nf)
      emitf("\tldp %s, %s, [sp], #16\n", vname(fregs[i], 8), vname(fregs[i + 1], 8));
    else
      emitf("\tldr %s, [sp], #16\n", vname(fregs[i], 8));
  }
  for (int i = ((n + 1) / 2 - 1) * 2; i >= 0; i -= 2) {
    if (i + 1 < n)
      emitf("\tldp %s, %s, [sp], #16\n", xname(regs[i], 8), xname(regs[i + 1], 8));
    else
      emitf("\tldr %s, [sp], #16\n", xname(regs[i], 8));
  }
}

static void prologue(void) {
  if (!needs_frame)
    return;
  emitf("\tstp x29, x30, [sp, #-16]!\n");
  emitf("\tmov x29, sp\n");
  save_pairs(false);
  if (fn->frame_size)
    emit_add_imm(R_SP, R_SP, -fn->frame_size, 8);
}

static void epilogue(void) {
  if (needs_frame) {
    if (fn->callee_area)
      emit_add_imm(R_SP, R_FP, -fn->callee_area, 8);
    else
      emitf("\tmov sp, x29\n");
    save_pairs(true);
    emitf("\tldp x29, x30, [sp], #16\n");
  }
  emitf("\tret\n");
}

//
// Instructions
//

static void emit_unary(const char *op, MInst *mi, int dsz, int ssz) {
  int rn = use(mi->rn, 0);
  int rd = def(mi->rd);
  emitf("\t%s %s, %s\n", op, rname(rd, dsz), rname(rn, ssz));
  finish_def();
}

static void emit_binary(const char *op, MInst *mi) {
  int rn = use(mi->rn, 0);
  int rm = use(mi->rm, 1);
  int rd = def(mi->rd);
  if (mi->shift)
    emitf("\t%s %s, %s, %s, lsl #%d\n", op, rname(rd, mi->sz), rname(rn, mi->sz),
          rname(rm, mi->sz), mi->shift);
  else
    emitf("\t%s %s, %s, %s\n", op, rname(rd, mi->sz), rname(rn, mi->sz), rname(rm, mi->sz));
  finish_def();
}

static void emit_memcpy(MInst *mi) {
  // dst base in x15 if spilled, src base in x17, data in x16
  bool u1, u2;
  int64_t n = mi->imm;
  int64_t done = 0;
  while (done < n) {
    int chunk = n - done >= 8 ? 8 : n - done >= 4 ? 4 : n - done >= 2 ? 2 : 1;
    int memty = chunk == 8 ? IR_I64 : chunk == 4 ? IR_I32 : chunk == 2 ? IR_I16 : IR_I8;
    MemRef s = mi->mem2;
    s.off += done;
    MemRef d = mi->mem;
    d.off += done;
    char *so = xstrdup(mem_operand(s, chunk, R_X17, 1, &u1));
    emitf("\t%s %s, %s\n", ldr_mnemonic(memty, false, u1), mem_reg(R_X16, memty), so);
    char *dop = xstrdup(mem_operand(d, chunk, R_X15, 2, &u2));
    emitf("\t%s %s, %s\n", str_mnemonic(memty, u2), mem_reg(R_X16, memty), dop);
    done += chunk;
  }
}

static void emit_memzero(MInst *mi) {
  bool u;
  int64_t n = mi->imm;
  int64_t done = 0;
  while (done < n) {
    int chunk = n - done >= 8 ? 8 : n - done >= 4 ? 4 : n - done >= 2 ? 2 : 1;
    int memty = chunk == 8 ? IR_I64 : chunk == 4 ? IR_I32 : chunk == 2 ? IR_I16 : IR_I8;
    MemRef d = mi->mem;
    d.off += done;
    char *dop = mem_operand(d, chunk, R_X17, 1, &u);
    emitf("\t%s %s, %s\n", str_mnemonic(memty, u), chunk == 8 ? "xzr" : "wzr", dop);
    done += chunk;
  }
}

static void emit_minst(Block *b, MInst *mi) {
  switch (mi->op) {
  case M_LABEL:
    emitf("%s:\n", block_label(b));
    return;
  case M_MOV:
    emit_unary("mov", mi, mi->sz, mi->sz);
    return;
  case M_FMOV:
    emit_unary("fmov", mi, mi->sz, mi->sz);
    return;
  case M_FMOV_GF:
  case M_FMOV_FG:
    emit_unary("fmov", mi, mi->sz, mi->sz);
    return;
  case M_MOVI: {
    int rd = def(mi->rd);
    emit_mov_imm(rd, mi->imm, mi->sz);
    finish_def();
    return;
  }
  case M_FMOVI: {
    int rd = def(mi->rd);
    double v;
    if (mi->sz == 4) {
      uint32_t b32 = (uint32_t)mi->imm;
      float f;
      memcpy(&f, &b32, 4);
      v = f;
    } else {
      memcpy(&v, &mi->imm, 8);
    }
    if (mi->imm == 0) {
      emitf("\tmovi %s, #0\n", vname(rd, 8));
    } else if (fmov_imm_ok(v, mi->sz)) {
      emitf("\tfmov %s, #%.17g\n", vname(rd, mi->sz), v);
    } else {
      emit_mov_imm(R_X15, mi->imm, mi->sz);
      emitf("\tfmov %s, %s\n", vname(rd, mi->sz), xname(R_X15, mi->sz));
    }
    finish_def();
    return;
  }
  case M_ADDR: {
    int rd = def(mi->rd);
    Sym *s = mi->sym;
    bool local = s->is_defined || s->is_string || (s->kind == SYM_FUNC && s->body);
    if (local) {
      emitf("\tadrp %s, %s@PAGE\n", xname(rd, 8), s->asm_name);
      emitf("\tadd %s, %s, %s@PAGEOFF\n", xname(rd, 8), xname(rd, 8), s->asm_name);
    } else {
      emitf("\tadrp %s, %s@GOTPAGE\n", xname(rd, 8), s->asm_name);
      emitf("\tldr %s, [%s, %s@GOTPAGEOFF]\n", xname(rd, 8), xname(rd, 8), s->asm_name);
    }
    if (mi->imm)
      emit_add_imm(rd, rd, mi->imm, 8);
    finish_def();
    return;
  }
  case M_FRAME: {
    int rd = def(mi->rd);
    emit_add_imm(rd, R_SP, slot_offset(mi->mem.slot) + mi->imm, 8);
    finish_def();
    return;
  }
  case M_ARGADDR: {
    int rd = def(mi->rd);
    emit_add_imm(rd, R_FP, 16 + mi->imm, 8);
    finish_def();
    return;
  }
  case M_ADD:
    emit_binary("add", mi);
    return;
  case M_SUB:
    emit_binary("sub", mi);
    return;
  case M_AND:
    emit_binary("and", mi);
    return;
  case M_ORR:
    emit_binary("orr", mi);
    return;
  case M_EOR:
    emit_binary("eor", mi);
    return;
  case M_MUL:
    emit_binary("mul", mi);
    return;
  case M_SDIV:
    emit_binary("sdiv", mi);
    return;
  case M_UDIV:
    emit_binary("udiv", mi);
    return;
  case M_LSL:
    emit_binary("lsl", mi);
    return;
  case M_LSR:
    emit_binary("lsr", mi);
    return;
  case M_ASR:
    emit_binary("asr", mi);
    return;
  case M_FADD:
    emit_binary("fadd", mi);
    return;
  case M_FSUB:
    emit_binary("fsub", mi);
    return;
  case M_FMUL:
    emit_binary("fmul", mi);
    return;
  case M_FDIV:
    emit_binary("fdiv", mi);
    return;
  case M_MADD:
  case M_MSUB: {
    int rn = use(mi->rn, 0);
    int rm = use(mi->rm, 1);
    int ra = use(mi->ra, 2);
    int rd = def(mi->rd);
    emitf("\t%s %s, %s, %s, %s\n", mi->op == M_MADD ? "madd" : "msub", xname(rd, mi->sz),
          xname(rn, mi->sz), xname(rm, mi->sz), xname(ra, mi->sz));
    finish_def();
    return;
  }
  case M_ADDI: {
    int rn = use(mi->rn, 0);
    int rd = def(mi->rd);
    emit_add_imm(rd, rn, mi->imm, mi->sz);
    finish_def();
    return;
  }
  case M_ANDI:
  case M_ORRI:
  case M_EORI: {
    int rn = use(mi->rn, 0);
    int rd = def(mi->rd);
    const char *op = mi->op == M_ANDI ? "and" : mi->op == M_ORRI ? "orr" : "eor";
    uint64_t v = mi->sz == 4 ? (uint32_t)mi->imm : (uint64_t)mi->imm;
    emitf("\t%s %s, %s, #0x%llx\n", op, xname(rd, mi->sz), xname(rn, mi->sz),
          (unsigned long long)v);
    finish_def();
    return;
  }
  case M_LSLI:
  case M_LSRI:
  case M_ASRI: {
    int rn = use(mi->rn, 0);
    int rd = def(mi->rd);
    const char *op = mi->op == M_LSLI ? "lsl" : mi->op == M_LSRI ? "lsr" : "asr";
    emitf("\t%s %s, %s, #%lld\n", op, xname(rd, mi->sz), xname(rn, mi->sz), (long long)mi->imm);
    finish_def();
    return;
  }
  case M_NEG:
    emit_unary("neg", mi, mi->sz, mi->sz);
    return;
  case M_MVN:
    emit_unary("mvn", mi, mi->sz, mi->sz);
    return;
  case M_FNEG:
    emit_unary("fneg", mi, mi->sz, mi->sz);
    return;
  case M_CMP: {
    int rn = use(mi->rn, 0);
    int rm = use(mi->rm, 1);
    emitf("\tcmp %s, %s\n", xname(rn, mi->sz), xname(rm, mi->sz));
    return;
  }
  case M_CMPI: {
    int rn = use(mi->rn, 0);
    int64_t v = mi->imm;
    if (v < 0)
      emitf("\tcmn %s, #%lld\n", xname(rn, mi->sz), (long long)-v);
    else
      emitf("\tcmp %s, #%lld\n", xname(rn, mi->sz), (long long)v);
    return;
  }
  case M_FCMP: {
    int rn = use(mi->rn, 0);
    int rm = use(mi->rm, 1);
    emitf("\tfcmp %s, %s\n", vname(rn, mi->sz), vname(rm, mi->sz));
    return;
  }
  case M_FCMPZ: {
    int rn = use(mi->rn, 0);
    emitf("\tfcmp %s, #0.0\n", vname(rn, mi->sz));
    return;
  }
  case M_CSET: {
    int rd = def(mi->rd);
    emitf("\tcset %s, %s\n", xname(rd, 4), cond_name[(int)mi->cond]);
    finish_def();
    return;
  }
  case M_SXTB:
    emit_unary("sxtb", mi, 4, 4);
    return;
  case M_SXTH:
    emit_unary("sxth", mi, 4, 4);
    return;
  case M_SXTW:
    emit_unary("sxtw", mi, 8, 4);
    return;
  case M_UXTB:
    emit_unary("uxtb", mi, 4, 4);
    return;
  case M_UXTH:
    emit_unary("uxth", mi, 4, 4);
    return;
  case M_UXTW:
    emit_unary("mov", mi, 4, 4); // writing a w register clears the upper half
    return;
  case M_SCVTF:
    emit_unary("scvtf", mi, mi->sz, mi->sz2);
    return;
  case M_UCVTF:
    emit_unary("ucvtf", mi, mi->sz, mi->sz2);
    return;
  case M_FCVTZS:
    emit_unary("fcvtzs", mi, mi->sz, mi->sz2);
    return;
  case M_FCVTZU:
    emit_unary("fcvtzu", mi, mi->sz, mi->sz2);
    return;
  case M_FCVT:
    emit_unary("fcvt", mi, mi->sz, mi->sz2);
    return;
  case M_LDR: {
    bool u;
    char *m = xstrdup(mem_operand(mi->mem, access_size(mi->memty), R_X17, 1, &u));
    int rd = def(mi->rd);
    emitf("\t%s %s, %s\n", ldr_mnemonic(mi->memty, mi->is_signed, u), mem_reg(rd, mi->memty), m);
    finish_def();
    return;
  }
  case M_STR: {
    int rv = use(mi->rn, 0);
    bool u;
    char *m = mem_operand(mi->mem, access_size(mi->memty), R_X17, 1, &u);
    emitf("\t%s %s, %s\n", str_mnemonic(mi->memty, u), mem_reg(rv, mi->memty), m);
    return;
  }
  case M_MEMCPY:
    emit_memcpy(mi);
    return;
  case M_MEMZERO:
    emit_memzero(mi);
    return;
  case M_BL:
    emitf("\tbl %s\n", mi->sym->asm_name);
    return;
  case M_BLR:
    emitf("\tblr x17\n");
    return;
  case M_B:
    if (!is_next_block(b, mi->target))
      emitf("\tb %s\n", block_label(mi->target));
    return;
  case M_BCOND: {
    // b.cond T; b F  -> if T is next: b.!cond F
    MInst *nx = mi->next;
    if (nx && nx->op == M_B && is_next_block(b, mi->target) && !is_next_block(b, nx->target)) {
      emitf("\tb.%s %s\n", cond_name[invert_cond(mi->cond)], block_label(nx->target));
      nx->target = mi->target; // the following b becomes a fall-through
      return;
    }
    emitf("\tb.%s %s\n", cond_name[(int)mi->cond], block_label(mi->target));
    return;
  }
  case M_CBZ:
  case M_CBNZ: {
    int rn = use(mi->rn, 0);
    MInst *nx = mi->next;
    bool z = mi->op == M_CBZ;
    if (nx && nx->op == M_B && is_next_block(b, mi->target) && !is_next_block(b, nx->target)) {
      emitf("\t%s %s, %s\n", z ? "cbnz" : "cbz", xname(rn, mi->sz), block_label(nx->target));
      nx->target = mi->target;
      return;
    }
    emitf("\t%s %s, %s\n", z ? "cbz" : "cbnz", xname(rn, mi->sz), block_label(mi->target));
    return;
  }
  case M_RET:
    epilogue();
    return;
  case M_PMOVE:
    emit_pmove(mi->pm);
    return;
  case M_JTAB: {
    int idx = use(mi->rn, 2);
    static int jt_counter;
    int id = jt_counter++;
    emitf("\tadr x16, LJTI%d\n", id);
    emitf("\tldrsw x17, [x16, %s, lsl #2]\n", xname(idx, 8));
    emitf("\tadd x16, x16, x17\n");
    emitf("\tbr x16\n");
    emitf("\t.p2align 2\nLJTI%d:\n\t.data_region jt32\n", id);
    for (int k = 0; k < mi->njt; k++)
      emitf("\t.long %s-LJTI%d\n", block_label(mi->jt[k]), id);
    emitf("\t.end_data_region\n");
    return;
  }
  case M_TRAP:
    emitf("\tbrk #0x1\n");
    return;
  }
  ICE("cannot emit machine op %d", mi->op);
}

//
// Functions and data
//

static bool section_text;
static const char *source_name;

void emit_begin(FILE *o, const char *src_name) {
  out = o;
  source_name = src_name;
  section_text = false;
  emitf("\t.build_version macos, 11, 0\n");
}

static bool frame_needed(Func *f) {
  if (f->has_calls || f->nslots > 0 || f->used_callee_gpr || f->used_callee_fpr ||
      f->frame_size > 0 || f->out_bytes > 0)
    return true;
  for (Block *b = f->entry; b; b = b->next)
    for (MInst *mi = b->mfirst; mi; mi = mi->next)
      if (mi->op == M_ARGADDR || mi->op == M_BL || mi->op == M_BLR)
        return true;
  return false;
}

void emit_func(Func *f) {
  fn = f;
  needs_frame = frame_needed(f);
  if (!section_text) {
    emitf("\t.section __TEXT,__text,regular,pure_instructions\n");
    section_text = true;
  }
  Sym *s = f->sym;
  if (s->is_extern_linkage) {
    emitf("\t.globl %s\n", f->name);
    if (s->is_inline && s->sc != SC_EXTERN)
      emitf("\t.weak_definition %s\n", f->name);
  }
  emitf("\t.p2align 2\n%s:\n", f->name);
  prologue();
  for (Block *b = f->entry; b; b = b->next)
    for (MInst *mi = b->mfirst; mi; mi = mi->next)
      emit_minst(b, mi);
}

static void emit_string_bytes(char *data, int64_t n) {
  emitf("\t.ascii \"");
  for (int64_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)data[i];
    if (c == '"' || c == '\\')
      emitf("\\%c", c);
    else if (c >= 32 && c < 127)
      emitf("%c", c);
    else
      emitf("\\%03o", c);
  }
  emitf("\"\n");
}

static bool is_cstring(Sym *s) {
  if (!s->is_string || s->ty->base->size != 1)
    return false;
  int64_t n = s->ty->size;
  for (int64_t i = 0; i + 1 < n; i++)
    if (!s->data[i])
      return false;
  return n > 0 && s->data[n - 1] == 0;
}

static void emit_object(Sym *s) {
  int64_t size = s->ty->size;
  int align = s->align > s->ty->align ? s->align : s->ty->align;
  if (size == 0)
    size = 1;
  int p2 = log2_u64(align > 0 ? align : 1);

  if (!s->data && !s->relocs) {
    if (s->is_extern_linkage)
      emitf("\t.globl %s\n", s->asm_name);
    emitf("\t.zerofill __DATA,__bss,%s,%lld,%d\n", s->asm_name, (long long)size, p2);
    return;
  }
  if (is_cstring(s)) {
    emitf("\t.section __TEXT,__cstring,cstring_literals\n");
    emitf("%s:\n", s->asm_name);
    emitf("\t.asciz \"");
    for (int64_t i = 0; i + 1 < s->ty->size; i++) {
      unsigned char c = (unsigned char)s->data[i];
      if (c == '"' || c == '\\')
        emitf("\\%c", c);
      else if (c >= 32 && c < 127)
        emitf("%c", c);
      else
        emitf("\\%03o", c);
    }
    emitf("\"\n");
    return;
  }
  bool readonly = (s->ty->qual & Q_CONST) || (s->ty->kind == TY_ARRAY && (s->ty->base->qual & Q_CONST)) ||
                  s->is_string;
  if (readonly && !s->relocs)
    emitf("\t.section __TEXT,__const\n");
  else if (readonly)
    emitf("\t.section __DATA,__const\n");
  else
    emitf("\t.section __DATA,__data\n");
  if (s->is_extern_linkage)
    emitf("\t.globl %s\n", s->asm_name);
  emitf("\t.p2align %d\n%s:\n", p2, s->asm_name);
  int64_t pos = 0;
  for (Reloc *r = s->relocs; r || pos < s->ty->size;) {
    int64_t next = r ? r->offset : s->ty->size;
    if (next > pos)
      emit_string_bytes(s->data + pos, next - pos);
    pos = next;
    if (!r)
      break;
    if (r->addend)
      emitf("\t.quad %s%+lld\n", r->sym->asm_name, (long long)r->addend);
    else
      emitf("\t.quad %s\n", r->sym->asm_name);
    pos += 8;
    r = r->next;
  }
  if (s->ty->size == 0)
    emitf("\t.byte 0\n");
}

void emit_data(Program *prog) {
  for (Sym *s = prog->syms; s; s = s->next) {
    if (s->kind != SYM_VAR || !s->is_defined)
      continue;
    if (s->is_string && !s->is_used)
      continue;
    emit_object(s);
  }
}

void emit_end(void) {
  (void)source_name;
}
