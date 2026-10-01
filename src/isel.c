// Instruction selection: IR -> machine instructions on virtual
// registers.
//
// Each IR value gets a virtual register.  Constants, symbol addresses
// and stack-slot addresses are not computed once and kept alive; they
// are rematerialised next to each use, where they often disappear into
// an immediate operand or an addressing mode.  A few patterns are
// matched across instructions:
//
//   * a comparison used only by the branch that ends its block becomes
//     cmp + b.cond (no 0/1 value is produced);
//   * base + constant addresses fold into load/store offsets;
//   * x + (y << k) uses the shifted-register form of add/sub;
//   * arithmetic with suitable constants uses immediate forms.
//
// Phi nodes become parallel moves at the end of each predecessor (the
// critical edges have been split, so those blocks have one successor).
// Calls become stack stores, a parallel move into the argument
// registers and a branch-and-link; the register allocator keeps values
// that live across a call in callee-saved registers.

#include "ouro.h"

static Func *fn;
static Block *cur_bb;

bool is_vreg(int r) {
  return r >= R_VREG;
}

int new_vreg(Func *f, int cls) {
  if (f->nvregs == f->vreg_cap) {
    int cap = f->vreg_cap ? f->vreg_cap * 2 : 256;
    f->vreg_cls = xgrow(f->vreg_cls, f->vreg_cap, cap);
    f->vreg_hint = xgrow(f->vreg_hint, sizeof(int) * f->vreg_cap, sizeof(int) * cap);
    f->vreg_cap = cap;
  }
  f->vreg_cls[f->nvregs] = (char)cls;
  f->vreg_hint[f->nvregs] = -1;
  return R_VREG + f->nvregs++;
}

static int cls_of(int irty) {
  return ir_is_float(irty) ? 1 : 0;
}

static int size_of(int irty) {
  return (irty == IR_I64 || irty == IR_F64) ? 8 : 4;
}

static MInst *new_mi(int op) {
  MInst *mi = xalloc(sizeof(MInst));
  mi->op = (short)op;
  mi->rd = mi->rn = mi->rm = mi->ra = R_NONE;
  mi->sz = 8;
  mi->mem.base = R_NONE;
  mi->mem.slot = -1;
  mi->mem.index = R_NONE;
  mi->mem2.base = R_NONE;
  mi->mem2.slot = -1;
  mi->mem2.index = R_NONE;
  return mi;
}

static MInst *add_mi(MInst *mi) {
  mi->prev = cur_bb->mlast;
  if (cur_bb->mlast)
    cur_bb->mlast->next = mi;
  else
    cur_bb->mfirst = mi;
  cur_bb->mlast = mi;
  return mi;
}

static MInst *mi_op(int op, int sz, int rd, int rn, int rm) {
  MInst *mi = new_mi(op);
  mi->sz = (char)sz;
  mi->rd = rd;
  mi->rn = rn;
  mi->rm = rm;
  return add_mi(mi);
}

static MInst *mi_imm(int op, int sz, int rd, int rn, int64_t imm) {
  MInst *mi = new_mi(op);
  mi->sz = (char)sz;
  mi->rd = rd;
  mi->rn = rn;
  mi->imm = imm;
  return add_mi(mi);
}

//
// Values
//

static int vreg_of(Inst *i) {
  if (i->vreg < 0)
    i->vreg = new_vreg(fn, cls_of(i->ty));
  return i->vreg;
}

static bool is_slot_addr(Inst *i) {
  return i->op == IR_ALLOCA;
}

// Materialises a floating value (constant, symbol or slot address) into
// a fresh register; returns the register holding any value.
static int reg_of(Inst *v) {
  switch (v->op) {
  case IR_ICONST: {
    int r = new_vreg(fn, 0);
    mi_imm(M_MOVI, size_of(v->ty), r, R_NONE, v->ival);
    return r;
  }
  case IR_FCONST: {
    int r = new_vreg(fn, 1);
    int64_t bits;
    if (v->ty == IR_F32) {
      float f = (float)v->fval;
      int32_t b32;
      memcpy(&b32, &f, 4);
      bits = (uint32_t)b32;
    } else {
      memcpy(&bits, &v->fval, 8);
    }
    mi_imm(M_FMOVI, size_of(v->ty), r, R_NONE, bits);
    return r;
  }
  case IR_GADDR: {
    int r = new_vreg(fn, 0);
    MInst *mi = mi_imm(M_ADDR, 8, r, R_NONE, v->ival);
    mi->sym = v->sym;
    return r;
  }
  case IR_ALLOCA: {
    int r = new_vreg(fn, 0);
    MInst *mi = mi_imm(M_FRAME, 8, r, R_NONE, 0);
    mi->mem.slot = v->aux;
    return r;
  }
  case IR_ARGADDR: {
    int r = new_vreg(fn, 0);
    mi_imm(M_ARGADDR, 8, r, R_NONE, v->ival);
    return r;
  }
  default:
    return vreg_of(v);
  }
}

// Like reg_of, but integer zero is the zero register.
static int reg_or_zr(Inst *v) {
  if (v->op == IR_ICONST && v->ival == 0)
    return R_ZR;
  return reg_of(v);
}

// Whether v (or -v, by switching between add and sub) is a 12-bit
// immediate, optionally shifted left by 12.
static bool fits_addsub_imm(int64_t v) {
  uint64_t a = v < 0 ? 0 - (uint64_t)v : (uint64_t)v; // INT64_MIN stays out of range
  return a <= 4095 || ((a & 0xFFF) == 0 && a <= (4095ULL << 12));
}

// Recognises base + (index << shift) where the index may be a 32-bit
// value extended to 64 bits: the register-offset addressing forms.
// `size` is the access size; the shift must be 0 or log2(size).
static bool index_pattern(Inst *a, int size, Inst **base, Inst **index, int *ext, int *shift) {
  if (a->op != IR_ADD || a->ty != IR_I64)
    return false;
  Inst *b = a->args[0];
  Inst *x = a->args[1];
  if (b->op == IR_ICONST || x->op == IR_ICONST)
    return false;
  int sh = 0;
  if (x->op == IR_SHL && x->args[1]->op == IR_ICONST) {
    sh = (int)x->args[1]->ival;
    if (sh != 0 && (1 << sh) != size)
      return false;
    x = x->args[0];
  }
  int e = 0;
  if (x->op == IR_SEXT32 || x->op == IR_ZEXT32) {
    e = x->op == IR_SEXT32 ? 1 : 2;
    x = x->args[0];
  }
  if (x->op == IR_ICONST)
    return false;
  *base = b;
  *index = x;
  *ext = e;
  *shift = sh;
  return true;
}

static int access_size_of(Inst *mem_user) {
  return ir_size(mem_user->memty);
}

// A memory operand for the address `a` of an access of `size` bytes.
static MemRef mem_of_sized(Inst *a, int size) {
  MemRef m;
  m.base = R_NONE;
  m.slot = -1;
  m.off = 0;
  m.index = R_NONE;
  m.ext = 0;
  m.shift = 0;
  int64_t off = 0;
  if (a->op == IR_ADD && a->args[1]->op == IR_ICONST && a->ty == IR_I64) {
    off = a->args[1]->ival;
    a = a->args[0];
  } else if (a->folded && size > 0) {
    Inst *base, *index;
    int ext, shift;
    if (index_pattern(a, size, &base, &index, &ext, &shift)) {
      if (is_slot_addr(base)) {
        m.slot = base->aux;
      } else {
        m.base = reg_of(base);
      }
      m.index = reg_of(index);
      m.ext = (char)ext;
      m.shift = (char)shift;
      return m;
    }
  }
  if (is_slot_addr(a)) {
    m.slot = a->aux;
    m.off = off;
    return m;
  }
  m.base = reg_of(a);
  m.off = off;
  return m;
}

static MemRef mem_of(Inst *a) {
  return mem_of_sized(a, 0);
}

//
// Folding decisions
//

static bool is_icmp(int op) {
  return op >= IR_EQ && op <= IR_UGE;
}

static bool is_fcmp(int op) {
  return op >= IR_FEQ && op <= IR_FGE;
}

static int cond_of(int op) {
  switch (op) {
  case IR_EQ:
  case IR_FEQ:
    return C_EQ;
  case IR_NE:
  case IR_FNE:
    return C_NE;
  case IR_SLT:
    return C_LT;
  case IR_SLE:
    return C_LE;
  case IR_SGT:
  case IR_FGT:
    return C_GT;
  case IR_SGE:
  case IR_FGE:
    return C_GE;
  case IR_ULT:
    return C_LO;
  case IR_ULE:
    return C_LS;
  case IR_UGT:
    return C_HI;
  case IR_UGE:
    return C_HS;
  case IR_FLT:
    return C_MI;
  case IR_FLE:
    return C_LS;
  default:
    ICE("not a comparison");
  }
}

// The condition after swapping the operands of the comparison.
static int swap_cond(int c) {
  switch (c) {
  case C_LT:
    return C_GT;
  case C_GT:
    return C_LT;
  case C_LE:
    return C_GE;
  case C_GE:
    return C_LE;
  case C_LO:
    return C_HI;
  case C_HI:
    return C_LO;
  case C_LS:
    return C_HS;
  case C_HS:
    return C_LS;
  default:
    return c;
  }
}

// Marks instructions that are emitted as part of their user.
static void decide_folding(void) {
  ir_compute_uses(fn);
  for (Block *b = fn->entry; b; b = b->next) {
    for (Inst *i = b->first; i; i = i->next) {
      i->folded = false;
      i->vreg = -1;
    }
    Inst *t = b->last;
    if (t->op == IR_BR) {
      Inst *c = t->args[0];
      if (c->bb == b && c->nuses == 1 && (is_icmp(c->op) || is_fcmp(c->op)))
        c->folded = true;
    }
  }
  // An address computation base + const whose every use is a load or
  // store address needs no instruction of its own.
  for (Block *b = fn->entry; b; b = b->next)
    for (Inst *i = b->first; i; i = i->next) {
      if (!(i->op == IR_ADD && i->ty == IR_I64 && i->args[1]->op == IR_ICONST))
        continue;
      bool all_mem = i->nuses > 0;
      for (int k = 0; k < i->nusers; k++) {
        Inst *u = i->users[k];
        bool as_addr = (u->op == IR_LOAD && u->args[0] == i) ||
                       (u->op == IR_STORE && u->args[1] == i && u->args[0] != i);
        if (!as_addr)
          all_mem = false;
      }
      if (all_mem)
        i->folded = true;
    }
  // base + (index << k) whose every use is the address of a load or
  // store of 2^k bytes becomes the register-offset addressing form; the
  // shift and extension inside fold too when nothing else uses them.
  for (Block *b = fn->entry; b; b = b->next)
    for (Inst *i = b->first; i; i = i->next) {
      if (i->folded || i->nuses == 0)
        continue;
      bool ok = true;
      for (int k = 0; k < i->nusers && ok; k++) {
        Inst *u = i->users[k];
        bool as_addr = (u->op == IR_LOAD && u->args[0] == i) ||
                       (u->op == IR_STORE && u->args[1] == i && u->args[0] != i);
        Inst *base, *index;
        int ext, shift;
        if (!as_addr || !index_pattern(i, access_size_of(u), &base, &index, &ext, &shift))
          ok = false;
      }
      if (!ok)
        continue;
      i->folded = true;
      Inst *x = i->args[1];
      if (x->op == IR_SHL && x->nuses == 1 && x->bb == b) {
        x->folded = true;
        x = x->args[0];
      }
      if ((x->op == IR_SEXT32 || x->op == IR_ZEXT32) && x->nuses == 1 && x->bb == b &&
          (i->args[1] == x || i->args[1]->folded))
        x->folded = true;
    }
  // x << k used only by an add or sub becomes a shifted operand.
  for (Block *b = fn->entry; b; b = b->next)
    for (Inst *i = b->first; i; i = i->next) {
      if (i->op != IR_SHL || i->args[1]->op != IR_ICONST || i->nuses != 1 || i->args[0]->op == IR_ICONST)
        continue;
      // Not when the other operand is a constant: sel_binary would swap
      // the operands of add, or select neg for 0 - x, and never use
      // the shifted form.
      Inst *u = i->users[0];
      if (u->bb == b && (u->op == IR_ADD || u->op == IR_SUB) && u->args[1] == i &&
          u->args[0] != i && u->args[0]->op != IR_ICONST && u->ty == i->ty && !u->folded &&
          !i->folded)
        i->folded = true;
    }
}

//
// Selection
//

static void sel_compare(Inst *c, int *cond) {
  Inst *a = c->args[0];
  Inst *b = c->args[1];
  int sz = size_of(a->ty);
  if (is_fcmp(c->op)) {
    *cond = cond_of(c->op);
    if (b->op == IR_FCONST && b->fval == 0) {
      mi_op(M_FCMPZ, sz, R_NONE, reg_of(a), R_NONE);
      return;
    }
    mi_op(M_FCMP, sz, R_NONE, reg_of(a), reg_of(b));
    return;
  }
  int cc = cond_of(c->op);
  if (a->op == IR_ICONST && b->op != IR_ICONST) {
    Inst *t = a;
    a = b;
    b = t;
    cc = swap_cond(cc);
  }
  *cond = cc;
  if (b->op == IR_ICONST && fits_addsub_imm(b->ival) && b->ival != INT64_MIN) {
    int64_t v = b->ival;
    if (sz == 4)
      v = (int32_t)v;
    mi_imm(M_CMPI, sz, R_NONE, reg_of(a), v);
    return;
  }
  mi_op(M_CMP, sz, R_NONE, reg_of(a), reg_or_zr(b));
}

static int shift_amount(Inst *i) {
  int bits = i->ty == IR_I64 ? 64 : 32;
  return (int)(i->args[1]->ival & (bits - 1));
}

static void sel_binary(Inst *i) {
  int sz = size_of(i->ty);
  int rd = vreg_of(i);
  Inst *a = i->args[0];
  Inst *b = i->args[1];
  switch (i->op) {
  case IR_ADD:
  case IR_SUB: {
    if (i->op == IR_ADD && a->op == IR_ICONST && b->op != IR_ICONST) {
      Inst *t = a;
      a = b;
      b = t;
    }
    if (b->op == IR_ICONST && fits_addsub_imm(b->ival)) {
      int64_t v = sz == 4 ? (int32_t)b->ival : b->ival;
      if (i->op == IR_SUB)
        v = -v;
      if (a->op == IR_ALLOCA) {
        MInst *mi = mi_imm(M_FRAME, 8, rd, R_NONE, v);
        mi->mem.slot = a->aux;
        return;
      }
      mi_imm(M_ADDI, sz, rd, reg_of(a), v);
      return;
    }
    if (i->op == IR_SUB && a->op == IR_ICONST && a->ival == 0) {
      mi_op(M_NEG, sz, rd, reg_of(b), R_NONE);
      return;
    }
    if (b->folded) { // b = x << k
      MInst *mi = mi_op(i->op == IR_ADD ? M_ADD : M_SUB, sz, rd, reg_or_zr(a), reg_of(b->args[0]));
      mi->shift = shift_amount(b);
      return;
    }
    mi_op(i->op == IR_ADD ? M_ADD : M_SUB, sz, rd, reg_or_zr(a), reg_of(b));
    return;
  }
  case IR_MUL:
    mi_op(M_MUL, sz, rd, reg_of(a), reg_of(b));
    return;
  case IR_SDIV:
  case IR_UDIV:
    mi_op(i->op == IR_SDIV ? M_SDIV : M_UDIV, sz, rd, reg_of(a), reg_of(b));
    return;
  case IR_SREM:
  case IR_UREM: {
    int ra = reg_of(a);
    int rb = reg_of(b);
    int q = new_vreg(fn, 0);
    mi_op(i->op == IR_SREM ? M_SDIV : M_UDIV, sz, q, ra, rb);
    MInst *mi = mi_op(M_MSUB, sz, rd, q, rb);
    mi->ra = ra;
    return;
  }
  case IR_AND:
  case IR_OR:
  case IR_XOR: {
    if (a->op == IR_ICONST && b->op != IR_ICONST) {
      Inst *t = a;
      a = b;
      b = t;
    }
    uint32_t enc;
    if (b->op == IR_ICONST && encode_logical_imm((uint64_t)b->ival, sz, &enc)) {
      int op = i->op == IR_AND ? M_ANDI : i->op == IR_OR ? M_ORRI : M_EORI;
      mi_imm(op, sz, rd, reg_of(a), sz == 4 ? (int64_t)(uint32_t)b->ival : b->ival);
      return;
    }
    int op = i->op == IR_AND ? M_AND : i->op == IR_OR ? M_ORR : M_EOR;
    mi_op(op, sz, rd, reg_or_zr(a), reg_or_zr(b));
    return;
  }
  case IR_SHL:
  case IR_LSHR:
  case IR_ASHR:
    if (b->op == IR_ICONST) {
      int op = i->op == IR_SHL ? M_LSLI : i->op == IR_LSHR ? M_LSRI : M_ASRI;
      mi_imm(op, sz, rd, reg_of(a), shift_amount(i));
      return;
    }
    mi_op(i->op == IR_SHL ? M_LSL : i->op == IR_LSHR ? M_LSR : M_ASR, sz, rd, reg_of(a),
          reg_of(b));
    return;
  case IR_FADD:
    mi_op(M_FADD, sz, rd, reg_of(a), reg_of(b));
    return;
  case IR_FSUB:
    mi_op(M_FSUB, sz, rd, reg_of(a), reg_of(b));
    return;
  case IR_FMUL:
    mi_op(M_FMUL, sz, rd, reg_of(a), reg_of(b));
    return;
  case IR_FDIV:
    mi_op(M_FDIV, sz, rd, reg_of(a), reg_of(b));
    return;
  default:
    ICE("sel_binary: %s", ir_op_name[i->op]);
  }
}

static void sel_load(Inst *i) {
  MInst *mi = new_mi(M_LDR);
  mi->rd = vreg_of(i);
  mi->memty = i->memty;
  mi->is_signed = i->is_signed;
  mi->mem = mem_of_sized(i->args[0], ir_size(i->memty));
  mi->sz = (char)size_of(i->ty);
  add_mi(mi);
}

static void sel_store(Inst *i) {
  Inst *v = i->args[0];
  int rv = ir_is_float(v->ty) ? reg_of(v) : reg_or_zr(v);
  MInst *mi = new_mi(M_STR);
  mi->rn = rv;
  mi->memty = i->memty;
  mi->mem = mem_of_sized(i->args[1], ir_size(i->memty));
  add_mi(mi);
}

static PMove *new_pmove(int cap) {
  PMove *pm = xalloc(sizeof(PMove));
  pm->dst = xalloc(sizeof(int) * (cap + 1));
  pm->src = xalloc(sizeof(int) * (cap + 1));
  pm->imm = xalloc(sizeof(int64_t) * (cap + 1));
  pm->cls = xalloc(cap + 1);
  pm->sz = xalloc(cap + 1);
  return pm;
}

static void pm_add(PMove *pm, int dst, int src, int64_t imm, int cls, int sz) {
  pm->dst[pm->n] = dst;
  pm->src[pm->n] = src;
  pm->imm[pm->n] = imm;
  pm->cls[pm->n] = (char)cls;
  pm->sz[pm->n] = (char)sz;
  pm->n++;
}

// Adds "dst <- v" to a parallel move; constants become immediates or
// are materialised beforehand.
static void pm_add_value(PMove *pm, int dst, Inst *v) {
  int cls = cls_of(v->ty);
  int sz = size_of(v->ty);
  if (v->op == IR_ICONST) {
    pm_add(pm, dst, -1, v->ival, 0, sz);
    return;
  }
  pm_add(pm, dst, reg_of(v), 0, cls, sz);
}

static MInst *emit_pmove(PMove *pm) {
  MInst *mi = new_mi(M_PMOVE);
  mi->pm = pm;
  return add_mi(mi);
}

static void hint(int vreg, int phys) {
  if (is_vreg(vreg) && fn->vreg_hint[vreg - R_VREG] < 0)
    fn->vreg_hint[vreg - R_VREG] = phys;
}

// Emits a call to a runtime function with integer arguments already in
// registers (used for large memcpy / memset).
static void call_helper(const char *name, int *args, int64_t *imms, int n) {
  PMove *pm = new_pmove(n);
  for (int k = 0; k < n; k++)
    pm_add(pm, R_X0 + k, args[k], imms[k], 0, 8);
  emit_pmove(pm);
  MInst *bl = new_mi(M_BL);
  static Sym *helpers[2];
  int h = str_eq(name, "_memcpy") ? 0 : 1;
  if (!helpers[h]) {
    helpers[h] = xalloc(sizeof(Sym));
    helpers[h]->asm_name = (char *)name;
    helpers[h]->kind = SYM_FUNC;
  }
  bl->sym = helpers[h];
  add_mi(bl);
  fn->has_calls = true;
}

static int addr_reg(Inst *a) {
  return reg_of(a);
}

static void sel_memcpy(Inst *i) {
  if (i->ival <= 128) {
    MInst *mi = new_mi(M_MEMCPY);
    mi->mem = mem_of(i->args[0]);  // destination
    mi->mem2 = mem_of(i->args[1]); // source
    mi->imm = i->ival;
    add_mi(mi);
    return;
  }
  int args[3] = {addr_reg(i->args[0]), addr_reg(i->args[1]), -1};
  int64_t imms[3] = {0, 0, i->ival};
  call_helper("_memcpy", args, imms, 3);
}

static void sel_memzero(Inst *i) {
  if (i->ival <= 128) {
    MInst *mi = new_mi(M_MEMZERO);
    mi->mem = mem_of(i->args[0]);
    mi->imm = i->ival;
    add_mi(mi);
    return;
  }
  int args[3] = {addr_reg(i->args[0]), -1, -1};
  int64_t imms[3] = {0, 0, i->ival};
  call_helper("_memset", args, imms, 3);
}

static void sel_call(Inst *i) {
  CallInfo *ci = i->call;
  int first = ci->indirect ? 1 : 0;
  // Stack arguments first: their stores use only scratch registers.
  for (int k = 0; k < ci->nlocs; k++) {
    ArgLoc *l = &ci->locs[k];
    if (l->kind != LOC_STACK)
      continue;
    Inst *v = i->args[first + k];
    MInst *mi = new_mi(M_STR);
    mi->rn = ir_is_float(v->ty) ? reg_of(v) : reg_or_zr(v);
    mi->memty = l->memty;
    mi->mem.base = R_SP;
    mi->mem.off = l->off;
    add_mi(mi);
  }
  PMove *pm = new_pmove(ci->nlocs + 1);
  for (int k = 0; k < ci->nlocs; k++) {
    ArgLoc *l = &ci->locs[k];
    if (l->kind == LOC_STACK)
      continue;
    Inst *v = i->args[first + k];
    int phys = l->kind == LOC_GPR ? R_X0 + l->reg : R_V0 + l->reg;
    pm_add_value(pm, phys, v);
    if (v->op != IR_ICONST)
      hint(pm->src[pm->n - 1], phys);
  }
  if (ci->indirect)
    pm_add_value(pm, R_X17, i->args[0]);
  emit_pmove(pm);
  MInst *call = new_mi(ci->indirect ? M_BLR : M_BL);
  call->sym = i->sym;
  call->rn = ci->indirect ? R_X17 : R_NONE;
  add_mi(call);
  fn->has_calls = true;

  switch (ci->ret_kind) {
  case RET_SCALAR: {
    PMove *r = new_pmove(1);
    int phys = ir_is_float(i->ty) ? R_V0 : R_X0;
    pm_add(r, vreg_of(i), phys, 0, cls_of(i->ty), size_of(i->ty));
    hint(vreg_of(i), phys);
    emit_pmove(r);
    break;
  }
  case RET_GPRS:
  case RET_FPRS: {
    Inst *buf = i->args[i->nargs - 1];
    int esz = ir_size(ci->ret_elem);
    for (int k = 0; k < ci->ret_n; k++) {
      MInst *st = new_mi(M_STR);
      st->rn = (ci->ret_kind == RET_GPRS ? R_X0 : R_V0) + k;
      st->memty = ci->ret_elem;
      st->mem = mem_of(buf);
      st->mem.off += k * esz;
      add_mi(st);
    }
    break;
  }
  default:
    break;
  }
}

static void sel_ret(Inst *i) {
  if (i->nargs > 0) {
    PMove *pm = new_pmove(i->nargs);
    for (int k = 0; k < i->nargs; k++) {
      Inst *v = i->args[k];
      int phys = (ir_is_float(v->ty) ? R_V0 : R_X0) + k;
      pm_add_value(pm, phys, v);
      if (v->op != IR_ICONST)
        hint(pm->src[pm->n - 1], phys);
    }
    emit_pmove(pm);
  }
  add_mi(new_mi(M_RET));
}

// Copies for the phis of `succ` at the end of the current block.
static void phi_moves(Block *succ) {
  int n = 0;
  for (Inst *p = succ->first; p && p->op == IR_PHI; p = p->next)
    n++;
  if (!n)
    return;
  PMove *pm = new_pmove(n);
  for (Inst *p = succ->first; p && p->op == IR_PHI; p = p->next) {
    Inst *v = NULL;
    for (int k = 0; k < p->nargs; k++)
      if (p->blocks[k] == cur_bb)
        v = p->args[k];
    ASSERT(v);
    pm_add_value(pm, vreg_of(p), v);
    // Prefer the phi's register for the incoming value: then the copy
    // disappears (the allocator resolves a hint to another virtual
    // register when it assigns this one).
    if (pm->src[pm->n - 1] >= R_VREG)
      hint(pm->src[pm->n - 1], vreg_of(p));
  }
  emit_pmove(pm);
}

static MInst *branch(int op, Block *target) {
  MInst *mi = new_mi(op);
  mi->target = target;
  return add_mi(mi);
}

static void sel_switch(Inst *i) {
  Inst *v = i->args[0];
  int sz = size_of(v->ty);
  int n = i->nblocks - 1;
  Block *dflt = i->blocks[0];
  if (n == 0) {
    branch(M_B, dflt);
    return;
  }
  int64_t lo = i->case_vals[0];
  int64_t hi = i->case_vals[n - 1];
  uint64_t range = (uint64_t)hi - (uint64_t)lo + 1; // no signed overflow

  int rv = reg_of(v);

  if (n >= 4 && range <= (uint64_t)n * 3 && range <= 4096) {
    // Jump table: rebase, bounds check, indirect branch.
    int idx = rv;
    if (lo != 0) {
      idx = new_vreg(fn, 0);
      if (fits_addsub_imm(lo)) {
        mi_imm(M_ADDI, sz, idx, rv, -lo);
      } else {
        int c = new_vreg(fn, 0);
        mi_imm(M_MOVI, sz, c, R_NONE, lo);
        mi_op(M_SUB, sz, idx, rv, c);
      }
    } else if (sz == 4) {
      idx = new_vreg(fn, 0);
      mi_op(M_UXTW, 8, idx, rv, R_NONE); // the table index must be 64-bit
    }
    if (fits_addsub_imm((int64_t)range - 1)) {
      mi_imm(M_CMPI, sz, R_NONE, idx, (int64_t)range - 1);
    } else {
      int c = new_vreg(fn, 0);
      mi_imm(M_MOVI, sz, c, R_NONE, (int64_t)range - 1);
      mi_op(M_CMP, sz, R_NONE, idx, c);
    }
    MInst *b = branch(M_BCOND, dflt);
    b->cond = C_HI;
    MInst *jt = new_mi(M_JTAB);
    jt->rn = idx;
    jt->sz = (char)sz;
    jt->njt = (int)range;
    jt->jt = xalloc(sizeof(Block *) * (range + 1));
    for (uint64_t k = 0; k < range; k++)
      jt->jt[k] = dflt;
    for (int k = 0; k < n; k++)
      jt->jt[(uint64_t)i->case_vals[k] - (uint64_t)lo] = i->blocks[k + 1];
    add_mi(jt);
    return;
  }

  // A chain of comparisons.
  for (int k = 0; k < n; k++) {
    int64_t c = i->case_vals[k];
    if (fits_addsub_imm(c) && c != INT64_MIN) {
      mi_imm(M_CMPI, sz, R_NONE, rv, sz == 4 ? (int32_t)c : c);
    } else {
      int cr = new_vreg(fn, 0);
      mi_imm(M_MOVI, sz, cr, R_NONE, c);
      mi_op(M_CMP, sz, R_NONE, rv, cr);
    }
    MInst *b = branch(M_BCOND, i->blocks[k + 1]);
    b->cond = C_EQ;
  }
  branch(M_B, dflt);
}

static void sel_br(Inst *t) {
  Inst *c = t->args[0];
  Block *tb = t->blocks[0];
  Block *fb = t->blocks[1];
  // The condition must be materialised before the branch is appended:
  // reg_of may emit instructions.
  if (c->folded) {
    // cmp a, #0 followed by b.eq/b.ne is cbz/cbnz.
    if (is_icmp(c->op) && (c->op == IR_EQ || c->op == IR_NE) && c->args[1]->op == IR_ICONST &&
        c->args[1]->ival == 0 && c->args[0]->op != IR_ICONST) {
      int r = reg_of(c->args[0]);
      MInst *mi = branch(c->op == IR_NE ? M_CBNZ : M_CBZ, tb);
      mi->rn = r;
      mi->sz = (char)size_of(c->args[0]->ty);
      branch(M_B, fb);
      return;
    }
    int cond;
    sel_compare(c, &cond);
    MInst *b = branch(M_BCOND, tb);
    b->cond = (char)cond;
    branch(M_B, fb);
    return;
  }
  int r = reg_of(c);
  MInst *mi = branch(M_CBNZ, tb);
  mi->rn = r;
  mi->sz = (char)size_of(c->ty);
  branch(M_B, fb);
}

static void sel_inst(Inst *i) {
  switch (i->op) {
  case IR_NOP:
  case IR_ICONST:
  case IR_FCONST:
  case IR_GADDR:
  case IR_ALLOCA:
  case IR_ARGADDR:
  case IR_PARAM:
  case IR_PHI:
    return;
  case IR_COPY:
    mi_op(ir_is_float(i->ty) ? M_FMOV : M_MOV, size_of(i->ty), vreg_of(i), reg_or_zr(i->args[0]),
          R_NONE);
    return;
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
  case IR_FADD:
  case IR_FSUB:
  case IR_FMUL:
  case IR_FDIV:
    sel_binary(i);
    return;
  case IR_NEG:
    mi_op(M_NEG, size_of(i->ty), vreg_of(i), reg_of(i->args[0]), R_NONE);
    return;
  case IR_NOT:
    mi_op(M_MVN, size_of(i->ty), vreg_of(i), reg_of(i->args[0]), R_NONE);
    return;
  case IR_FNEG:
    mi_op(M_FNEG, size_of(i->ty), vreg_of(i), reg_of(i->args[0]), R_NONE);
    return;
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
  case IR_FEQ:
  case IR_FNE:
  case IR_FLT:
  case IR_FLE:
  case IR_FGT:
  case IR_FGE: {
    int cond;
    sel_compare(i, &cond);
    MInst *mi = mi_op(M_CSET, 4, vreg_of(i), R_NONE, R_NONE);
    mi->cond = (char)cond;
    return;
  }
  case IR_SEXT8:
    mi_op(M_SXTB, 4, vreg_of(i), reg_of(i->args[0]), R_NONE);
    return;
  case IR_SEXT16:
    mi_op(M_SXTH, 4, vreg_of(i), reg_of(i->args[0]), R_NONE);
    return;
  case IR_ZEXT8:
    mi_op(M_UXTB, 4, vreg_of(i), reg_of(i->args[0]), R_NONE);
    return;
  case IR_ZEXT16:
    mi_op(M_UXTH, 4, vreg_of(i), reg_of(i->args[0]), R_NONE);
    return;
  case IR_SEXT32:
    mi_op(M_SXTW, 8, vreg_of(i), reg_of(i->args[0]), R_NONE);
    return;
  case IR_ZEXT32:
    mi_op(M_UXTW, 8, vreg_of(i), reg_of(i->args[0]), R_NONE);
    return;
  case IR_TRUNC:
    mi_op(M_MOV, 4, vreg_of(i), reg_of(i->args[0]), R_NONE);
    return;
  case IR_SITOF:
  case IR_UITOF: {
    MInst *mi = mi_op(i->op == IR_SITOF ? M_SCVTF : M_UCVTF, size_of(i->ty), vreg_of(i),
                      reg_of(i->args[0]), R_NONE);
    mi->sz2 = (char)size_of(i->args[0]->ty);
    return;
  }
  case IR_FTOSI:
  case IR_FTOUI: {
    MInst *mi = mi_op(i->op == IR_FTOSI ? M_FCVTZS : M_FCVTZU, size_of(i->ty), vreg_of(i),
                      reg_of(i->args[0]), R_NONE);
    mi->sz2 = (char)size_of(i->args[0]->ty);
    return;
  }
  case IR_FEXT:
  case IR_FTRUNC: {
    MInst *mi = mi_op(M_FCVT, size_of(i->ty), vreg_of(i), reg_of(i->args[0]), R_NONE);
    mi->sz2 = (char)size_of(i->args[0]->ty);
    return;
  }
  case IR_BITCAST:
    mi_op(ir_is_float(i->ty) ? M_FMOV_GF : M_FMOV_FG, size_of(i->ty), vreg_of(i),
          reg_of(i->args[0]), R_NONE);
    return;
  case IR_LOAD:
    sel_load(i);
    return;
  case IR_STORE:
    sel_store(i);
    return;
  case IR_MEMCPY:
    sel_memcpy(i);
    return;
  case IR_MEMZERO:
    sel_memzero(i);
    return;
  case IR_CALL:
    sel_call(i);
    return;
  case IR_JMP:
    phi_moves(i->blocks[0]);
    branch(M_B, i->blocks[0]);
    return;
  case IR_BR:
    sel_br(i);
    return;
  case IR_SWITCH:
    sel_switch(i);
    return;
  case IR_RET:
    sel_ret(i);
    return;
  case IR_UNREACHABLE:
    add_mi(new_mi(M_TRAP));
    return;
  case IR_DYNALLOC: {
    int size = reg_of(i->args[0]);
    mi_op(M_DYNALLOC, 8, vreg_of(i), size, R_NONE);
    return;
  }
  case IR_STACKSAVE:
    mi_op(M_STACKSAVE, 8, vreg_of(i), R_NONE, R_NONE);
    return;
  case IR_STACKRESTORE:
    mi_op(M_STACKRESTORE, 8, R_NONE, reg_of(i->args[0]), R_NONE);
    return;
  }
  ICE("cannot select %s", ir_op_name[i->op]);
}

// Machine-level dead code: folding a value into addressing modes or
// shifted operands can leave the instruction that computed it on its
// own (when the IR value had several users, all folded).  Remove pure
// instructions whose result nothing reads, until none is left.
static bool mi_is_pure_def(MInst *mi) {
  switch (mi->op) {
  case M_MOV:
  case M_FMOV:
  case M_MOVI:
  case M_FMOVI:
  case M_FMOV_GF:
  case M_FMOV_FG:
  case M_ADDR:
  case M_FRAME:
  case M_ARGADDR:
  case M_ADD:
  case M_SUB:
  case M_AND:
  case M_ORR:
  case M_EOR:
  case M_ADDI:
  case M_ANDI:
  case M_ORRI:
  case M_EORI:
  case M_MUL:
  case M_MADD:
  case M_MSUB:
  case M_SDIV:
  case M_UDIV:
  case M_LSL:
  case M_LSR:
  case M_ASR:
  case M_LSLI:
  case M_LSRI:
  case M_ASRI:
  case M_NEG:
  case M_MVN:
  case M_SXTB:
  case M_SXTH:
  case M_SXTW:
  case M_UXTB:
  case M_UXTH:
  case M_UXTW:
  case M_FADD:
  case M_FSUB:
  case M_FMUL:
  case M_FDIV:
  case M_FNEG:
  case M_SCVTF:
  case M_UCVTF:
  case M_FCVTZS:
  case M_FCVTZU:
  case M_FCVT:
  case M_CSET:
    return is_vreg(mi->rd);
  default:
    return false;
  }
}

static void mir_dce(Func *f) {
  int *count = xalloc(sizeof(int) * (f->nvregs + 1));
  int uses[64], defs[64];
  for (Block *b = f->entry; b; b = b->next)
    for (MInst *mi = b->mfirst; mi; mi = mi->next) {
      if (mi->op == M_PMOVE) {
        for (int k = 0; k < mi->pm->n; k++)
          if (is_vreg(mi->pm->src[k]))
            count[mi->pm->src[k] - R_VREG]++;
        continue;
      }
      int nu, nd;
      minst_regs(mi, uses, &nu, defs, &nd);
      for (int k = 0; k < nu; k++)
        if (is_vreg(uses[k]))
          count[uses[k] - R_VREG]++;
    }
  bool changed = true;
  while (changed) {
    changed = false;
    for (Block *b = f->entry; b; b = b->next)
      for (MInst *mi = b->mlast; mi;) {
        MInst *prev = mi->prev;
        if (mi_is_pure_def(mi) && count[mi->rd - R_VREG] == 0) {
          int nu, nd;
          minst_regs(mi, uses, &nu, defs, &nd);
          for (int k = 0; k < nu; k++)
            if (is_vreg(uses[k]))
              count[uses[k] - R_VREG]--;
          if (mi->prev)
            mi->prev->next = mi->next;
          else
            b->mfirst = mi->next;
          if (mi->next)
            mi->next->prev = mi->prev;
          else
            b->mlast = mi->prev;
          changed = true;
        }
        mi = prev;
      }
  }
}

void isel_func(Func *f) {
  fn = f;
  f->nvregs = 0;
  decide_folding();

  // Stack slots for allocas.
  int nslots = 0;
  for (Inst *i = f->entry->first; i; i = i->next)
    if (i->op == IR_ALLOCA)
      nslots++;
  for (Block *b = f->entry->next; b; b = b->next)
    for (Inst *i = b->first; i; i = i->next)
      if (i->op == IR_ALLOCA)
        ICE("alloca outside the entry block");
  f->slot_size = xalloc(sizeof(int) * (nslots + 1));
  f->slot_align = xalloc(sizeof(int) * (nslots + 1));
  f->nslots = 0;
  for (Inst *i = f->entry->first; i; i = i->next)
    if (i->op == IR_ALLOCA) {
      i->aux = f->nslots;
      f->slot_size[f->nslots] = (int)i->ival;
      f->slot_align[f->nslots] = i->align;
      f->nslots++;
    }
  f->nallocas = f->nslots;

  int label = 0;
  for (Block *b = f->entry; b; b = b->next) {
    b->label = label++;
    b->mfirst = b->mlast = NULL;
  }

  for (Block *b = f->entry; b; b = b->next) {
    cur_bb = b;
    add_mi(new_mi(M_LABEL))->target = b;
    if (b == f->entry) {
      // Incoming register arguments, all at once.
      int n = 0;
      for (Inst *i = b->first; i; i = i->next)
        if (i->op == IR_PARAM)
          n++;
      if (n) {
        PMove *pm = new_pmove(n);
        for (Inst *i = b->first; i; i = i->next)
          if (i->op == IR_PARAM) {
            int phys = (ir_is_float(i->ty) ? R_V0 : R_X0) + (int)i->ival;
            pm_add(pm, vreg_of(i), phys, 0, cls_of(i->ty), size_of(i->ty));
            hint(vreg_of(i), phys);
          }
        emit_pmove(pm);
      }
    }
    for (Inst *i = b->first; i; i = i->next)
      if (!i->folded)
        sel_inst(i);
  }
  mir_dce(f);
}

//
// Logical immediates (the bitmask operand of AND/ORR/EOR)
//

// A logical immediate is an element of 2, 4, ..., 64 bits, replicated
// across the register, whose bits form a single run of ones when read
// circularly (C6.2.13 in the Arm ARM).  Only validity is needed here;
// the assembler does the encoding.
bool encode_logical_imm(uint64_t v, int sz, uint32_t *enc) {
  *enc = 0;
  if (sz == 4)
    v = (v & 0xFFFFFFFFULL) | (v << 32);
  if (v == 0 || v == ~0ULL)
    return false;
  int size = 64;
  while (size > 2) {
    int half = size / 2;
    uint64_t mask = (1ULL << half) - 1;
    if ((v & mask) != ((v >> half) & mask))
      break;
    size = half;
  }
  // Count the 0 -> 1 transitions around the element: exactly one for a
  // rotated run of ones.
  int rises = 0;
  for (int i = 0; i < size; i++) {
    int bit = (int)((v >> i) & 1);
    int prev = (int)((v >> ((i + size - 1) % size)) & 1);
    if (bit && !prev)
      rises++;
  }
  return rises == 1;
}
