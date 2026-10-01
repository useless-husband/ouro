// Lowers the typed AST of one function to IR.
//
// Every local variable starts as a stack slot (IR_ALLOCA) accessed by
// loads and stores; mem2reg later turns the slots whose address does not
// escape into SSA values.  Expression temporaries are SSA values from
// the start.  Aggregates are handled by address: the "value" of a struct
// expression is a pointer to its storage.
//
// This is also where the Apple arm64 calling convention is applied: each
// argument is split into scalar pieces with an explicit register or
// stack location, so the back end never sees C types.

#include "ouro.h"

typedef struct {
  Inst *addr;
  Type *ty;
  Member *bf; // the bit-field, if the lvalue is one
  bool is_volatile;
} LVal;

static Func *fn;
static Sym *fsym;
static Block *cur;
static Block *entry;
static Inst *last_alloca;
static Block *break_bb;
static Block *continue_bb;
static Inst *sret_slot;
static int64_t va_area_off;

// Blocks that declare variable length arrays save the stack pointer on
// entry and restore it on exit.  break and continue leave such blocks
// too, so each saved value remembers how many loops and switches
// enclosed its block.
typedef struct {
  Inst *slot;
  int breakable; // enclosing loops and switches
  int loops;     // enclosing loops
} VlaScope;
static VlaScope vla_scopes[256];
static int nvla;
static int breakable_depth;
static int loop_depth;

static Inst *gen_expr(Expr *e);
static Inst *gen_addr(Expr *e);
static void gen_stmt(Stmt *s);
static void gen_cond(Expr *e, Block *t, Block *f);
static void gen_local_init(Inst *addr, Init *init, Type *ty);

//
// Building blocks
//

static bool terminated(void) {
  return cur->last && ir_is_terminator(cur->last->op);
}

static Inst *emit(int op, int ty, int nargs) {
  Inst *i = ir_new(fn, op, ty, nargs);
  ir_append(cur, i);
  return i;
}

static Inst *iconst(int ty, int64_t v) {
  return ir_iconst(fn, ty, v);
}

static Inst *bin(int op, int ty, Inst *a, Inst *b) {
  Inst *i = emit(op, ty, 2);
  i->args[0] = a;
  i->args[1] = b;
  return i;
}

static Inst *un(int op, int ty, Inst *a) {
  Inst *i = emit(op, ty, 1);
  i->args[0] = a;
  return i;
}

static Block *new_block(void) {
  return ir_new_block(fn);
}

static void jmp(Block *b) {
  Inst *i = emit(IR_JMP, IR_VOID, 0);
  i->blocks = xalloc(sizeof(Block *));
  i->blocks[0] = b;
  i->nblocks = 1;
}

static void br(Inst *c, Block *t, Block *f) {
  Inst *i = emit(IR_BR, IR_VOID, 1);
  i->args[0] = c;
  i->blocks = xalloc(sizeof(Block *) * 2);
  i->blocks[0] = t;
  i->blocks[1] = f;
  i->nblocks = 2;
}

// Makes `b` the insertion point, falling through from the current block
// unless it already ended.
static void start_block(Block *b) {
  if (!terminated())
    jmp(b);
  ir_append_block(fn, b);
  cur = b;
}

// After a jump, code until the next label is unreachable; it goes into
// a block with no predecessors, which CFG cleanup deletes.
static void start_dead_block(void) {
  start_block(new_block());
}

// Stack slots live at the start of the entry block, so that they
// dominate every use and the frame layout is static.
static Inst *alloca_slot(int64_t size, int align, Ident *name) {
  Inst *i = ir_new(fn, IR_ALLOCA, IR_I64, 0);
  i->ival = size > 0 ? size : 1;
  i->align = align > 0 ? align : 1;
  i->var_name = name;
  if (!last_alloca) {
    ir_insert_front(entry, i);
  } else if (last_alloca->next) {
    ir_insert_before(last_alloca->next, i);
  } else {
    ir_append(entry, i);
  }
  last_alloca = i;
  return i;
}

static Inst *load(Inst *addr, int memty, bool is_signed, bool is_volatile) {
  int ty = (memty == IR_I8 || memty == IR_I16) ? IR_I32 : memty;
  Inst *i = un(IR_LOAD, ty, addr);
  i->memty = (char)memty;
  i->is_signed = is_signed;
  i->is_volatile = is_volatile;
  return i;
}

static void store(Inst *val, Inst *addr, int memty, bool is_volatile) {
  Inst *i = emit(IR_STORE, IR_VOID, 2);
  i->args[0] = val;
  i->args[1] = addr;
  i->memty = (char)memty;
  i->is_volatile = is_volatile;
}

static Inst *add_off(Inst *p, int64_t off) {
  if (off == 0)
    return p;
  return bin(IR_ADD, IR_I64, p, iconst(IR_I64, off));
}

static void memcpy_ir(Inst *dst, Inst *src, int64_t size) {
  if (size <= 0)
    return;
  Inst *i = emit(IR_MEMCPY, IR_VOID, 2);
  i->args[0] = dst;
  i->args[1] = src;
  i->ival = size;
  fn->has_calls |= size > 128; // large copies call memcpy
}

static void memzero_ir(Inst *dst, int64_t size) {
  if (size <= 0)
    return;
  Inst *i = emit(IR_MEMZERO, IR_VOID, 1);
  i->args[0] = dst;
  i->ival = size;
  fn->has_calls |= size > 128;
}

static Inst *global_addr(Sym *s) {
  Inst *g = ir_new(fn, IR_GADDR, IR_I64, 0);
  g->sym = s;
  s->is_used = true;
  return g;
}

static bool is_signed_type(Type *ty) {
  return is_integer(ty) && !ty->is_unsigned;
}

static Inst *load_scalar(Inst *addr, Type *ty, bool is_volatile) {
  return load(addr, ir_memtype_of(ty), is_signed_type(ty), is_volatile);
}

//
// Conversions
//

// Re-establishes the canonical form of an I32 that holds a value of a
// type narrower than int.
static Inst *narrow(Inst *v, Type *to) {
  if (to->kind == TY_BOOL)
    return v;
  if (to->size == 1)
    return un(to->is_unsigned ? IR_ZEXT8 : IR_SEXT8, IR_I32, v);
  if (to->size == 2)
    return un(to->is_unsigned ? IR_ZEXT16 : IR_SEXT16, IR_I32, v);
  return v;
}

static Inst *to_bool(Inst *v, Type *from) {
  if (from->kind == TY_BOOL)
    return v;
  if (is_flonum(from))
    return bin(IR_FNE, IR_I32, v, ir_fconst(fn, v->ty, 0));
  return bin(IR_NE, IR_I32, v, iconst(v->ty, 0));
}

static Inst *gen_cast(Inst *v, Type *from, Type *to) {
  if (to->kind == TY_VOID)
    return NULL;
  if (is_record(to) || is_record(from) || to->kind == TY_ARRAY)
    return v;
  if (from->kind == TY_ARRAY || from->kind == TY_FUNC)
    from = pointer_to(ty_void);
  int to_ir = ir_type_of(to);

  if (to->kind == TY_BOOL)
    return to_bool(v, from);

  if (is_flonum(from)) {
    if (is_flonum(to)) {
      if (v->ty == to_ir)
        return v;
      return un(to_ir == IR_F64 ? IR_FEXT : IR_FTRUNC, to_ir, v);
    }
    // Floating to integer.  Narrow targets convert through a 32-bit
    // signed integer, which is exact for every value in their range.
    bool u = to->is_unsigned && to->size >= 4 && to->kind != TY_PTR;
    Inst *r = un(u ? IR_FTOUI : IR_FTOSI, to_ir, v);
    if (to->size < 4)
      r = narrow(r, to);
    return r;
  }

  if (is_flonum(to)) {
    bool u = from->is_unsigned && from->kind != TY_PTR;
    if (v->ty == IR_I32 && u && from->size < 4)
      u = false; // canonical narrow unsigned values are non-negative
    return un(u ? IR_UITOF : IR_SITOF, to_ir, v);
  }

  // Integer or pointer to integer or pointer.
  if (to_ir == IR_I64) {
    if (v->ty == IR_I64)
      return v;
    return un(from->is_unsigned || from->kind == TY_BOOL ? IR_ZEXT32 : IR_SEXT32, IR_I64, v);
  }
  if (v->ty == IR_I64)
    v = un(IR_TRUNC, IR_I32, v);
  if (to->size >= 4)
    return v;
  int from_size = (is_integer(from) && from->size < 4) ? (int)from->size : 4;
  if (from->kind == TY_BOOL)
    return v;
  if (from_size < to->size && (from->is_unsigned || !to->is_unsigned))
    return v; // the value is already canonical for the wider type
  if (from_size == to->size && from->is_unsigned == to->is_unsigned)
    return v;
  return narrow(v, to);
}

//
// Lvalues
//

static bool is_volatile_lv(Expr *e) {
  return (e->ty->qual & Q_VOLATILE) != 0;
}

static LVal gen_lval(Expr *e) {
  LVal lv;
  lv.ty = e->ty;
  lv.bf = NULL;
  lv.is_volatile = is_volatile_lv(e);
  if (e->kind == E_MEMBER && e->member->is_bitfield) {
    lv.addr = add_off(gen_addr(e->l), e->member->offset);
    lv.bf = e->member;
    return lv;
  }
  lv.addr = gen_addr(e);
  return lv;
}

// Extracts a bit-field from its storage unit: shift it to the top, then
// back down with the field's signedness.
static Inst *bf_extract(Inst *word, Member *bf) {
  int w = word->ty == IR_I64 ? 64 : 32;
  int up = w - bf->bit_width - bf->bit_offset;
  int down = w - bf->bit_width;
  Inst *v = word;
  if (up)
    v = bin(IR_SHL, word->ty, v, iconst(word->ty, up));
  if (down)
    v = bin(is_signed_type(bf->ty) ? IR_ASHR : IR_LSHR, word->ty, v, iconst(word->ty, down));
  return v;
}

static Inst *load_lval(LVal lv) {
  if (lv.bf) {
    Inst *word = load(lv.addr, ir_memtype_of(lv.bf->ty), false, lv.is_volatile);
    return bf_extract(word, lv.bf);
  }
  if (is_aggregate(lv.ty) || lv.ty->kind == TY_FUNC)
    return lv.addr;
  return load_scalar(lv.addr, lv.ty, lv.is_volatile);
}

// Stores `val` and returns the value the lvalue now holds.
static Inst *store_lval(LVal lv, Inst *val) {
  if (!lv.bf) {
    store(val, lv.addr, ir_memtype_of(lv.ty), lv.is_volatile);
    return val;
  }
  Member *bf = lv.bf;
  int memty = ir_memtype_of(bf->ty);
  int ty = val->ty;
  uint64_t mask = bf->bit_width >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << bf->bit_width) - 1;
  Inst *word = load(lv.addr, memty, false, lv.is_volatile);
  Inst *cleared = bin(IR_AND, ty, word, iconst(ty, (int64_t) ~(mask << bf->bit_offset)));
  Inst *field = bin(IR_AND, ty, val, iconst(ty, (int64_t)mask));
  if (bf->bit_offset)
    field = bin(IR_SHL, ty, field, iconst(ty, bf->bit_offset));
  store(bin(IR_OR, ty, cleared, field), lv.addr, memty, lv.is_volatile);

  // The value of the assignment is the field as it reads back.
  int w = ty == IR_I64 ? 64 : 32;
  int sh = w - bf->bit_width;
  if (!sh)
    return val;
  Inst *r = bin(IR_SHL, ty, val, iconst(ty, sh));
  return bin(is_signed_type(bf->ty) ? IR_ASHR : IR_LSHR, ty, r, iconst(ty, sh));
}

static Inst *gen_addr(Expr *e) {
  switch (e->kind) {
  case E_VAR: {
    Sym *s = e->sym;
    if (s->is_local) {
      if (!s->slot)
        ICE("local '%s' has no stack slot", s->name ? s->name->name : "?");
      if (s->ty->is_vla)
        return load(s->slot, IR_I64, false, false); // the slot holds the array's address
      return s->slot;
    }
    return global_addr(s);
  }
  case E_STR:
    return global_addr(e->sym);
  case E_DEREF:
    return gen_expr(e->l);
  case E_MEMBER:
    return add_off(gen_addr(e->l), e->member->offset);
  case E_COMPLIT:
    if (e->sym->is_local) {
      gen_local_init(e->sym->slot, e->init, e->sym->ty);
      return e->sym->slot;
    }
    return global_addr(e->sym);
  case E_COMMA:
    gen_expr(e->l);
    return gen_addr(e->r);
  default:
    // A struct rvalue (call result, assignment, ?: ...) is represented by
    // the address of a temporary.
    if (is_record(e->ty) || e->ty->kind == TY_ARRAY)
      return gen_expr(e);
    break;
  }
  error_tok(e->tok, "expression is not an lvalue");
}

//
// Calls
//

typedef struct {
  Inst **vals;
  ArgLoc *locs;
  int n;
  int ngrn; // next general-purpose argument register
  int nsrn; // next SIMD/FP argument register
  int64_t nsaa; // next stacked argument offset
} ArgState;

static void add_piece(ArgState *st, Inst *v, int kind, int reg, int64_t off, int memty) {
  st->vals[st->n] = v;
  st->locs[st->n].kind = (char)kind;
  st->locs[st->n].reg = reg;
  st->locs[st->n].off = (int)off;
  st->locs[st->n].memty = (char)memty;
  st->n++;
}

// Loads an aggregate of at most 16 bytes as n 64-bit words.  A size that
// is not a multiple of 8 is first copied into a padded temporary, so the
// loads never read past the object.
static void load_words(Inst *addr, Type *ty, int n, Inst **out) {
  if (ty->size % 8) {
    Inst *tmp = alloca_slot(n * 8, 8, NULL);
    memcpy_ir(tmp, addr, ty->size);
    addr = tmp;
  }
  for (int i = 0; i < n; i++)
    out[i] = load(add_off(addr, i * 8), IR_I64, false, false);
}

static int stack_memty(Type *ty) {
  if (ty->kind == TY_PTR || ty->kind == TY_ARRAY || ty->kind == TY_FUNC)
    return IR_I64;
  return ir_memtype_of(ty);
}

static void pass_named(ArgState *st, Inst *v, Type *ty) {
  ArgClass c = abi_classify(ty);
  switch (c.kind) {
  case AC_NONE:
    return;
  case AC_INT:
    if (st->ngrn < 8) {
      add_piece(st, v, LOC_GPR, st->ngrn++, 0, IR_VOID);
    } else {
      // Apple: stack arguments are packed at their natural alignment.
      int64_t sz = ty->kind == TY_PTR ? 8 : ty->size;
      st->nsaa = align_up(st->nsaa, sz);
      add_piece(st, v, LOC_STACK, 0, st->nsaa, stack_memty(ty));
      st->nsaa += sz;
    }
    return;
  case AC_FP:
    if (st->nsrn < 8) {
      add_piece(st, v, LOC_FPR, st->nsrn++, 0, IR_VOID);
    } else {
      int sz = ir_size(c.elem);
      st->nsaa = align_up(st->nsaa, sz);
      add_piece(st, v, LOC_STACK, 0, st->nsaa, c.elem);
      st->nsaa += sz;
    }
    return;
  case AC_INTS: {
    Inst *words[2];
    load_words(v, ty, c.n, words);
    if (st->ngrn + c.n <= 8) {
      for (int i = 0; i < c.n; i++)
        add_piece(st, words[i], LOC_GPR, st->ngrn++, 0, IR_VOID);
    } else {
      // An aggregate is never split between registers and the stack.
      st->ngrn = 8;
      st->nsaa = align_up(st->nsaa, 8);
      for (int i = 0; i < c.n; i++)
        add_piece(st, words[i], LOC_STACK, 0, st->nsaa + i * 8, IR_I64);
      st->nsaa += c.n * 8;
    }
    return;
  }
  case AC_HFA: {
    int esz = ir_size(c.elem);
    Inst *elems[4];
    for (int i = 0; i < c.n; i++)
      elems[i] = load(add_off(v, i * esz), c.elem, false, false);
    if (st->nsrn + c.n <= 8) {
      for (int i = 0; i < c.n; i++)
        add_piece(st, elems[i], LOC_FPR, st->nsrn++, 0, IR_VOID);
    } else {
      // On the stack an HFA keeps its exact size and element alignment.
      st->nsrn = 8;
      st->nsaa = align_up(st->nsaa, esz);
      for (int i = 0; i < c.n; i++)
        add_piece(st, elems[i], LOC_STACK, 0, st->nsaa + i * esz, c.elem);
      st->nsaa += c.n * esz;
    }
    return;
  }
  case AC_INDIRECT: {
    // The callee gets the address of a private copy.
    Inst *tmp = alloca_slot(ty->size, ty->align, NULL);
    memcpy_ir(tmp, v, ty->size);
    if (st->ngrn < 8) {
      add_piece(st, tmp, LOC_GPR, st->ngrn++, 0, IR_VOID);
    } else {
      st->nsaa = align_up(st->nsaa, 8);
      add_piece(st, tmp, LOC_STACK, 0, st->nsaa, IR_I64);
      st->nsaa += 8;
    }
    return;
  }
  }
}

// Apple passes every variadic argument on the stack, in 8-byte slots.
static void pass_variadic(ArgState *st, Inst *v, Type *ty) {
  ArgClass c = abi_classify(ty);
  st->nsaa = align_up(st->nsaa, 8);
  switch (c.kind) {
  case AC_NONE:
    return;
  case AC_INT:
    if (v->ty == IR_I32)
      v = un(ty->is_unsigned ? IR_ZEXT32 : IR_SEXT32, IR_I64, v);
    add_piece(st, v, LOC_STACK, 0, st->nsaa, IR_I64);
    st->nsaa += 8;
    return;
  case AC_FP:
    add_piece(st, v, LOC_STACK, 0, st->nsaa, c.elem);
    st->nsaa += 8;
    return;
  case AC_INTS:
  case AC_HFA: {
    int n = (int)((ty->size + 7) / 8);
    Inst *addr = v;
    if (ty->size % 8) {
      Inst *tmp = alloca_slot(n * 8, 8, NULL);
      memcpy_ir(tmp, addr, ty->size);
      addr = tmp;
    }
    for (int i = 0; i < n; i++)
      add_piece(st, load(add_off(addr, i * 8), IR_I64, false, false), LOC_STACK, 0,
                st->nsaa + i * 8, IR_I64);
    st->nsaa += n * 8;
    return;
  }
  case AC_INDIRECT: {
    Inst *tmp = alloca_slot(ty->size, ty->align, NULL);
    memcpy_ir(tmp, v, ty->size);
    add_piece(st, tmp, LOC_STACK, 0, st->nsaa, IR_I64);
    st->nsaa += 8;
    return;
  }
  }
}

static Sym *direct_callee(Expr *f) {
  if ((f->kind == E_DECAY || f->kind == E_ADDR) && f->l->kind == E_VAR &&
      f->l->sym->kind == SYM_FUNC)
    return f->l->sym;
  return NULL;
}

static Inst *gen_call(Expr *e) {
  Type *ft = e->l->ty->base;
  Type *ret = e->ty;
  ArgClass rc = abi_classify(ret);

  ArgState st;
  int cap = e->nargs * 4 + 2;
  st.vals = xalloc(sizeof(Inst *) * cap);
  st.locs = xalloc(sizeof(ArgLoc) * cap);
  st.n = 0;
  st.ngrn = 0;
  st.nsrn = 0;
  st.nsaa = 0;

  Sym *callee_sym = direct_callee(e->l);
  Inst *callee = NULL;
  if (!callee_sym)
    callee = gen_expr(e->l);
  else
    callee_sym->is_used = true;

  // A large aggregate result is written by the callee to the buffer
  // whose address it receives in x8.
  Inst *retbuf = NULL;
  if (rc.kind == AC_INDIRECT) {
    retbuf = alloca_slot(ret->size, ret->align, NULL);
    add_piece(&st, retbuf, LOC_GPR, 8, 0, IR_VOID);
  }

  for (int i = 0; i < e->nargs; i++) {
    Expr *a = e->args[i];
    Inst *v = gen_expr(a);
    if (ft->variadic && i >= ft->nparams)
      pass_variadic(&st, v, a->ty);
    else
      pass_named(&st, v, a->ty);
  }

  int64_t stack_bytes = align_up(st.nsaa, 16);
  if (fn->out_bytes < stack_bytes)
    fn->out_bytes = (int)stack_bytes;
  fn->has_calls = true;

  CallInfo *ci = xalloc(sizeof(CallInfo));
  ci->locs = st.locs;
  ci->nlocs = st.n;
  ci->indirect = callee != NULL;
  ci->stack_bytes = (int)stack_bytes;

  int ty = IR_VOID;
  bool extra = false;
  switch (rc.kind) {
  case AC_NONE:
  case AC_INDIRECT:
    ci->ret_kind = RET_NONE;
    break;
  case AC_INT:
  case AC_FP:
    ci->ret_kind = RET_SCALAR;
    ty = ir_type_of(ret);
    break;
  case AC_INTS:
    ci->ret_kind = RET_GPRS;
    ci->ret_n = rc.n;
    ci->ret_elem = IR_I64;
    retbuf = alloca_slot(rc.n * 8, ret->align > 8 ? ret->align : 8, NULL);
    extra = true;
    break;
  case AC_HFA:
    ci->ret_kind = RET_FPRS;
    ci->ret_n = rc.n;
    ci->ret_elem = (char)rc.elem;
    retbuf = alloca_slot(ret->size, ret->align, NULL);
    extra = true;
    break;
  }

  int first = callee ? 1 : 0;
  Inst *call = emit(IR_CALL, ty, first + st.n + (extra ? 1 : 0));
  if (callee)
    call->args[0] = callee;
  for (int i = 0; i < st.n; i++)
    call->args[first + i] = st.vals[i];
  if (extra)
    call->args[first + st.n] = retbuf;
  call->sym = callee_sym;
  call->call = ci;

  if (rc.kind == AC_INT || rc.kind == AC_FP)
    return call;
  return retbuf; // NULL for void
}

//
// Expressions
//

static Inst *phi2(int ty, Inst *v1, Block *b1, Inst *v2, Block *b2) {
  Inst *p = emit(IR_PHI, ty, 2);
  p->args[0] = v1;
  p->args[1] = v2;
  p->blocks = xalloc(sizeof(Block *) * 2);
  p->blocks[0] = b1;
  p->blocks[1] = b2;
  p->nblocks = 2;
  return p;
}

static Inst *gen_bool_value(Expr *e) {
  Block *t = new_block();
  Block *f = new_block();
  Block *end = new_block();
  gen_cond(e, t, f);
  start_block(t);
  jmp(end);
  start_block(f);
  jmp(end);
  start_block(end);
  return phi2(IR_I32, iconst(IR_I32, 1), t, iconst(IR_I32, 0), f);
}

static int compare_op(ExprKind k, Type *ty) {
  if (is_flonum(ty)) {
    switch (k) {
    case E_EQ:
      return IR_FEQ;
    case E_NE:
      return IR_FNE;
    case E_LT:
      return IR_FLT;
    case E_LE:
      return IR_FLE;
    case E_GT:
      return IR_FGT;
    default:
      return IR_FGE;
    }
  }
  bool u = ty->is_unsigned || ty->kind == TY_PTR;
  switch (k) {
  case E_EQ:
    return IR_EQ;
  case E_NE:
    return IR_NE;
  case E_LT:
    return u ? IR_ULT : IR_SLT;
  case E_LE:
    return u ? IR_ULE : IR_SLE;
  case E_GT:
    return u ? IR_UGT : IR_SGT;
  default:
    return u ? IR_UGE : IR_SGE;
  }
}

static Inst *arith(ExprKind k, Type *ty, Inst *l, Inst *r) {
  int t = ir_type_of(ty);
  if (is_flonum(ty)) {
    switch (k) {
    case E_ADD:
      return bin(IR_FADD, t, l, r);
    case E_SUB:
      return bin(IR_FSUB, t, l, r);
    case E_MUL:
      return bin(IR_FMUL, t, l, r);
    case E_DIV:
      return bin(IR_FDIV, t, l, r);
    default:
      ICE("bad floating operator");
    }
  }
  bool u = ty->is_unsigned;
  switch (k) {
  case E_ADD:
    return bin(IR_ADD, t, l, r);
  case E_SUB:
    return bin(IR_SUB, t, l, r);
  case E_MUL:
    return bin(IR_MUL, t, l, r);
  case E_DIV:
    return bin(u ? IR_UDIV : IR_SDIV, t, l, r);
  case E_MOD:
    return bin(u ? IR_UREM : IR_SREM, t, l, r);
  case E_BITAND:
    return bin(IR_AND, t, l, r);
  case E_BITOR:
    return bin(IR_OR, t, l, r);
  case E_BITXOR:
    return bin(IR_XOR, t, l, r);
  case E_SHL:
    return bin(IR_SHL, t, l, r);
  case E_SHR:
    return bin(u ? IR_LSHR : IR_ASHR, t, l, r);
  default:
    ICE("bad integer operator %d", k);
  }
}

// The result of arithmetic on a narrow type must be put back into
// canonical form (the AST only computes in int or wider, so this
// matters for ++/-- and compound assignment).
static Inst *fit(Inst *v, Type *ty) {
  if (is_integer(ty) && ty->size < 4)
    return ty->kind == TY_BOOL ? to_bool(v, ty_int) : narrow(v, ty);
  return v;
}

static Inst *ptr_offset(Inst *p, Inst *idx, int64_t scale) {
  if (idx->op == IR_ICONST)
    return add_off(p, idx->ival * scale);
  Inst *off = scale == 1 ? idx : bin(IR_MUL, IR_I64, idx, iconst(IR_I64, scale));
  return bin(IR_ADD, IR_I64, p, off);
}

static Inst *gen_va_arg(Expr *e) {
  LVal lv = gen_lval(e->l);
  Inst *p = load(lv.addr, IR_I64, false, false);
  Type *ty = e->ty;
  ArgClass c = abi_classify(ty);
  if (c.kind == AC_INDIRECT) {
    store(add_off(p, 8), lv.addr, IR_I64, false);
    return load(p, IR_I64, false, false);
  }
  store(add_off(p, align_up(ty->size, 8)), lv.addr, IR_I64, false);
  if (is_record(ty))
    return p;
  return load_scalar(p, ty, false);
}

static Inst *gen_expr(Expr *e) {
  switch (e->kind) {
  case E_ICONST:
    return iconst(ir_type_of(e->ty), e->ival);
  case E_FCONST:
    return ir_fconst(fn, ir_type_of(e->ty), e->fval);
  case E_STR:
    return global_addr(e->sym);
  case E_VAR:
  case E_MEMBER:
  case E_DEREF:
  case E_COMPLIT:
    if (is_aggregate(e->ty) || e->ty->kind == TY_FUNC)
      return gen_addr(e);
    return load_lval(gen_lval(e));
  case E_DECAY:
  case E_ADDR:
    return gen_addr(e->l);
  case E_CAST: {
    Inst *v = gen_expr(e->l);
    if (e->ty->kind == TY_VOID)
      return NULL;
    return gen_cast(v, e->l->ty, e->ty);
  }
  case E_NEG: {
    Inst *v = gen_expr(e->l);
    return un(is_flonum(e->ty) ? IR_FNEG : IR_NEG, v->ty, v);
  }
  case E_BITNOT: {
    Inst *v = gen_expr(e->l);
    return un(IR_NOT, v->ty, v);
  }
  case E_NOT: {
    Inst *v = gen_expr(e->l);
    if (is_flonum(e->l->ty))
      return bin(IR_FEQ, IR_I32, v, ir_fconst(fn, v->ty, 0));
    return bin(IR_EQ, IR_I32, v, iconst(v->ty, 0));
  }
  case E_ADD:
  case E_SUB:
  case E_MUL:
  case E_DIV:
  case E_MOD:
  case E_SHL:
  case E_SHR:
  case E_BITAND:
  case E_BITOR:
  case E_BITXOR: {
    Inst *l = gen_expr(e->l);
    Inst *r = gen_expr(e->r);
    return arith(e->kind, e->ty, l, r);
  }
  case E_EQ:
  case E_NE:
  case E_LT:
  case E_LE:
  case E_GT:
  case E_GE: {
    Inst *l = gen_expr(e->l);
    Inst *r = gen_expr(e->r);
    return bin(compare_op(e->kind, e->l->ty), IR_I32, l, r);
  }
  case E_PTRADD: {
    Inst *p = gen_expr(e->l);
    Inst *i = gen_expr(e->r);
    return ptr_offset(p, i, e->scale);
  }
  case E_PTRDIFF: {
    Inst *l = gen_expr(e->l);
    Inst *r = gen_expr(e->r);
    Inst *d = bin(IR_SUB, IR_I64, l, r);
    if (e->scale == 1)
      return d;
    // The difference is an exact multiple of the element size.
    if (is_pow2(e->scale))
      return bin(IR_ASHR, IR_I64, d, iconst(IR_I64, log2_u64(e->scale)));
    return bin(IR_SDIV, IR_I64, d, iconst(IR_I64, e->scale));
  }
  case E_LOGAND:
  case E_LOGOR:
    return gen_bool_value(e);
  case E_COND: {
    Block *t = new_block();
    Block *f = new_block();
    Block *end = new_block();
    gen_cond(e->cond, t, f);
    start_block(t);
    Inst *v1 = gen_expr(e->l);
    Block *t_end = cur;
    jmp(end);
    start_block(f);
    Inst *v2 = gen_expr(e->r);
    Block *f_end = cur;
    jmp(end);
    start_block(end);
    if (e->ty->kind == TY_VOID || !v1 || !v2)
      return NULL;
    if (is_record(e->ty)) {
      // Both arms are addresses of struct values; the result must be a
      // value of its own (the arms may be temporaries).
      Inst *p = phi2(IR_I64, v1, t_end, v2, f_end);
      Inst *tmp = alloca_slot(e->ty->size, e->ty->align, NULL);
      memcpy_ir(tmp, p, e->ty->size);
      return tmp;
    }
    return phi2(v1->ty, v1, t_end, v2, f_end);
  }
  case E_COMMA:
    gen_expr(e->l);
    return gen_expr(e->r);
  case E_ASSIGN: {
    if (is_record(e->ty)) {
      Inst *src = gen_expr(e->r);
      Inst *dst = gen_addr(e->l);
      memcpy_ir(dst, src, e->ty->size);
      return dst;
    }
    Inst *v = gen_expr(e->r);
    LVal lv = gen_lval(e->l);
    return store_lval(lv, v);
  }
  case E_OPASSIGN: {
    LVal lv = gen_lval(e->l);
    Inst *old = load_lval(lv);
    Inst *r = gen_expr(e->r);
    Inst *res;
    if (e->op == E_PTRADD) {
      res = ptr_offset(old, r, e->scale);
    } else {
      Inst *o = gen_cast(old, e->l->ty, e->cty);
      res = arith((ExprKind)e->op, e->cty, o, r);
      res = gen_cast(res, e->cty, e->l->ty);
    }
    return store_lval(lv, res);
  }
  case E_PREINC:
  case E_POSTINC: {
    LVal lv = gen_lval(e->l);
    Inst *old = load_lval(lv);
    Type *ty = e->l->ty;
    Inst *nv;
    if (is_ptr(ty)) {
      nv = add_off(old, e->ival * e->scale);
      if (nv == old)
        nv = bin(IR_ADD, IR_I64, old, iconst(IR_I64, 0));
    } else if (is_flonum(ty)) {
      nv = bin(IR_FADD, old->ty, old, ir_fconst(fn, old->ty, (double)e->ival));
    } else if (ty->kind == TY_BOOL) {
      nv = e->ival > 0 ? iconst(IR_I32, 1) : bin(IR_XOR, IR_I32, old, iconst(IR_I32, 1));
    } else {
      nv = bin(IR_ADD, old->ty, old, iconst(old->ty, e->ival));
      if (!lv.bf)
        nv = fit(nv, ty);
    }
    Inst *stored = store_lval(lv, nv);
    return e->kind == E_PREINC ? stored : old;
  }
  case E_CALL:
    return gen_call(e);
  case E_STMTEXPR: {
    Stmt *body = e->body;
    for (int i = 0; i + 1 < body->nstmts; i++)
      gen_stmt(body->stmts[i]);
    if (body->nstmts == 0)
      return NULL;
    Stmt *last = body->stmts[body->nstmts - 1];
    if (last->kind == S_EXPR)
      return gen_expr(last->expr);
    gen_stmt(last);
    return NULL;
  }
  case E_VA_START: {
    LVal lv = gen_lval(e->l);
    Inst *area = emit(IR_ARGADDR, IR_I64, 0);
    area->ival = va_area_off;
    store(area, lv.addr, IR_I64, false);
    return NULL;
  }
  case E_VA_ARG:
    return gen_va_arg(e);
  case E_BUILTIN_TRAP:
    emit(IR_UNREACHABLE, IR_VOID, 0);
    start_dead_block();
    return NULL;
  }
  error_tok(e->tok, "cannot generate code for this expression");
}

// Evaluates `e` as a branch condition.  && || ! become control flow
// directly instead of materialising 0 or 1.
static void gen_cond(Expr *e, Block *t, Block *f) {
  switch (e->kind) {
  case E_LOGAND: {
    Block *mid = new_block();
    gen_cond(e->l, mid, f);
    start_block(mid);
    gen_cond(e->r, t, f);
    return;
  }
  case E_LOGOR: {
    Block *mid = new_block();
    gen_cond(e->l, t, mid);
    start_block(mid);
    gen_cond(e->r, t, f);
    return;
  }
  case E_NOT:
    gen_cond(e->l, f, t);
    return;
  case E_ICONST:
    jmp(e->ival ? t : f);
    return;
  default:
    break;
  }
  Inst *v = gen_expr(e);
  if (!v)
    error_tok(e->tok, "statement requires expression of scalar type ('void' invalid)");
  if (ir_is_float(v->ty))
    v = bin(IR_FNE, IR_I32, v, ir_fconst(fn, v->ty, 0));
  br(v, t, f);
}

//
// Local initialisation
//

// Stores the explicitly initialised parts of `init` into the object at
// `addr`; the caller has zeroed the object when parts are missing.
static void init_parts(Inst *addr, Init *init, Type *ty, Member *bf) {
  if (!init)
    return;
  if (init->expr) {
    Expr *e = init->expr;
    if (e->kind == E_STR && ty->kind == TY_ARRAY) {
      int64_t n = e->tok->str_bytes;
      if (n > ty->size)
        n = ty->size;
      memcpy_ir(addr, global_addr(e->sym), n);
      return;
    }
    if (is_record(ty)) {
      memcpy_ir(addr, gen_expr(e), ty->size);
      return;
    }
    Inst *v = gen_expr(e);
    if (bf) {
      LVal lv;
      lv.addr = addr;
      lv.ty = ty;
      lv.bf = bf;
      lv.is_volatile = false;
      store_lval(lv, v);
      return;
    }
    store(v, addr, ir_memtype_of(ty), (ty->qual & Q_VOLATILE) != 0);
    return;
  }
  if (ty->kind == TY_ARRAY) {
    for (int64_t i = 0; i < init->nelems; i++)
      if (init->elems[i])
        init_parts(add_off(addr, i * ty->base->size), init->elems[i], ty->base, NULL);
    return;
  }
  if (is_record(ty)) {
    for (Member *m = ty->rec->members; m; m = m->next) {
      if (ty->kind == TY_UNION && init->union_mem != m)
        continue;
      Init *c = init->elems[m->idx];
      if (c)
        init_parts(add_off(addr, m->offset), c, m->ty, m->is_bitfield ? m : NULL);
    }
  }
}

// Whether every byte of the object is written by the initializer, so no
// zeroing is needed first.
static bool fully_initialized(Init *init, Type *ty) {
  if (!init)
    return false;
  if (init->expr)
    return !(init->expr->kind == E_STR && ty->kind == TY_ARRAY &&
             init->expr->tok->str_bytes < ty->size);
  if (ty->kind == TY_ARRAY) {
    for (int64_t i = 0; i < ty->len; i++)
      if (i >= init->nelems || !fully_initialized(init->elems[i], ty->base))
        return false;
    return true;
  }
  if (ty->kind == TY_STRUCT) {
    int64_t end = 0;
    for (Member *m = ty->rec->members; m; m = m->next) {
      if (m->is_bitfield || m->offset != end)
        return false; // padding or shared storage: zero it
      if (!fully_initialized(init->elems[m->idx], m->ty))
        return false;
      end = m->offset + m->ty->size;
    }
    return end == ty->size;
  }
  return false;
}

static void gen_local_init(Inst *addr, Init *init, Type *ty) {
  if (is_aggregate(ty) && !fully_initialized(init, ty))
    memzero_ir(addr, ty->size);
  init_parts(addr, init, ty, NULL);
}

//
// Statements
//

static Block *label_block(Label *l) {
  if (!l->bb)
    l->bb = new_block();
  return l->bb;
}

// The number of values in a case range minus one, computed without
// signed overflow (a range may span all 64-bit values).
static uint64_t case_span(Stmt *c) {
  return (uint64_t)c->case_hi - (uint64_t)c->case_lo;
}

static int cmp_case(const void *a, const void *b) {
  Stmt *x = *(Stmt **)a;
  Stmt *y = *(Stmt **)b;
  if (x->case_lo < y->case_lo)
    return -1;
  return x->case_lo > y->case_lo;
}

static void gen_switch(Stmt *s) {
  Inst *v = gen_expr(s->expr);
  Block *end = new_block();
  for (int i = 0; i < s->ncases; i++)
    s->cases[i]->bb = new_block();
  Block *dflt = end;
  if (s->default_stmt) {
    s->default_stmt->bb = new_block();
    dflt = s->default_stmt->bb;
  }

  // Wide GNU case ranges are tested with comparisons before the
  // dispatch; everything else becomes one IR_SWITCH.
  int64_t nvals = 0;
  for (int i = 0; i < s->ncases; i++) {
    Stmt *c = s->cases[i];
    if (case_span(c) < 64)
      nvals += (int64_t)case_span(c) + 1;
  }
  int vty = v->ty;
  for (int i = 0; i < s->ncases; i++) {
    Stmt *c = s->cases[i];
    if (case_span(c) < 64)
      continue;
    Inst *d = bin(IR_SUB, vty, v, iconst(vty, c->case_lo));
    Inst *in = bin(IR_ULE, IR_I32, d, iconst(vty, (int64_t)case_span(c)));
    Block *next = new_block();
    br(in, c->bb, next);
    start_block(next);
  }

  Stmt **cases = xalloc(sizeof(Stmt *) * (s->ncases + 1));
  for (int i = 0; i < s->ncases; i++)
    cases[i] = s->cases[i];
  qsort(cases, s->ncases, sizeof(Stmt *), cmp_case); // values are distinct

  Inst *sw = emit(IR_SWITCH, IR_VOID, 1);
  sw->args[0] = v;
  sw->blocks = xalloc(sizeof(Block *) * (nvals + 1));
  sw->case_vals = xalloc(sizeof(int64_t) * (nvals + 1));
  sw->blocks[0] = dflt;
  int n = 1;
  for (int i = 0; i < s->ncases; i++) {
    Stmt *c = cases[i];
    if (case_span(c) >= 64)
      continue;
    // Counted, not compared: x <= case_hi would never fail for INT64_MAX.
    for (uint64_t k = 0; k <= case_span(c); k++) {
      sw->case_vals[n - 1] = (int64_t)((uint64_t)c->case_lo + k);
      sw->blocks[n++] = c->bb;
    }
  }
  sw->nblocks = n;

  Block *saved = break_bb;
  break_bb = end;
  start_dead_block(); // statements before the first label are unreachable
  breakable_depth++;
  gen_stmt(s->then);
  breakable_depth--;
  break_bb = saved;
  start_block(end);
}

static void gen_return(Stmt *s) {
  Type *ret = fsym->ty->ret;
  ArgClass rc = abi_classify(ret);
  Inst *v = NULL;
  if (s->expr)
    v = gen_expr(s->expr);
  if (!v || ret->kind == TY_VOID) {
    emit(IR_RET, IR_VOID, 0);
    return;
  }
  switch (rc.kind) {
  case AC_NONE:
    emit(IR_RET, IR_VOID, 0);
    return;
  case AC_INT:
  case AC_FP: {
    Inst *r = emit(IR_RET, IR_VOID, 1);
    r->args[0] = v;
    return;
  }
  case AC_INTS: {
    Inst *words[2];
    load_words(v, ret, rc.n, words);
    Inst *r = emit(IR_RET, IR_VOID, rc.n);
    for (int i = 0; i < rc.n; i++)
      r->args[i] = words[i];
    return;
  }
  case AC_HFA: {
    int esz = ir_size(rc.elem);
    Inst *elems[4];
    for (int i = 0; i < rc.n; i++)
      elems[i] = load(add_off(v, i * esz), rc.elem, false, false);
    Inst *r = emit(IR_RET, IR_VOID, rc.n);
    for (int i = 0; i < rc.n; i++)
      r->args[i] = elems[i];
    return;
  }
  case AC_INDIRECT: {
    Inst *dst = load(sret_slot, IR_I64, false, false);
    memcpy_ir(dst, v, ret->size);
    emit(IR_RET, IR_VOID, 0);
    return;
  }
  }
}

// Falling off the end of a function: main returns 0 (C11 5.1.2.2.3);
// for other functions the value is unspecified, and 0 is as good as any.
static void gen_fallthrough_return(void) {
  Type *ret = fsym->ty->ret;
  ArgClass rc = abi_classify(ret);
  switch (rc.kind) {
  case AC_INT: {
    Inst *r = emit(IR_RET, IR_VOID, 1);
    r->args[0] = iconst(ir_type_of(ret), 0);
    return;
  }
  case AC_FP: {
    Inst *r = emit(IR_RET, IR_VOID, 1);
    r->args[0] = ir_fconst(fn, ir_type_of(ret), 0);
    return;
  }
  case AC_INTS: {
    Inst *r = emit(IR_RET, IR_VOID, rc.n);
    for (int i = 0; i < rc.n; i++)
      r->args[i] = iconst(IR_I64, 0);
    return;
  }
  case AC_HFA: {
    Inst *r = emit(IR_RET, IR_VOID, rc.n);
    for (int i = 0; i < rc.n; i++)
      r->args[i] = ir_fconst(fn, rc.elem, 0);
    return;
  }
  default:
    emit(IR_RET, IR_VOID, 0);
    return;
  }
}

static void stack_restore(Inst *slot) {
  Inst *saved = load(slot, IR_I64, false, false); // before the restore that uses it
  Inst *r = emit(IR_STACKRESTORE, IR_VOID, 1);
  r->args[0] = saved;
}

// Before a break (or continue) leaves VLA blocks, release their stack.
static void restore_for_exit(bool is_continue) {
  for (int k = 0; k < nvla; k++) {
    bool inside = is_continue ? vla_scopes[k].loops == loop_depth
                              : vla_scopes[k].breakable == breakable_depth;
    if (inside) {
      stack_restore(vla_scopes[k].slot);
      return; // the outermost one restores everything inside it
    }
  }
}

static void gen_block(Stmt *s) {
  if (!s->has_vla) {
    for (int i = 0; i < s->nstmts; i++)
      gen_stmt(s->stmts[i]);
    return;
  }
  if (nvla == 256)
    error_tok(s->tok, "variable length arrays nested too deeply");
  Inst *slot = alloca_slot(8, 8, NULL);
  store(emit(IR_STACKSAVE, IR_I64, 0), slot, IR_I64, false);
  vla_scopes[nvla].slot = slot;
  vla_scopes[nvla].breakable = breakable_depth;
  vla_scopes[nvla].loops = loop_depth;
  nvla++;
  for (int i = 0; i < s->nstmts; i++)
    gen_stmt(s->stmts[i]);
  nvla--;
  if (!terminated())
    stack_restore(slot);
}

static void gen_stmt(Stmt *s) {
  switch (s->kind) {
  case S_NULL:
    return;
  case S_VLA: {
    Type *ty = s->var->ty;
    Inst *n = gen_expr(ty->vla_len);
    store(n, ty->vla_count->slot, IR_I64, false);
    Inst *bytes = bin(IR_MUL, IR_I64, n, iconst(IR_I64, ty->base->size));
    Inst *p = emit(IR_DYNALLOC, IR_I64, 1);
    p->args[0] = bytes;
    store(p, s->var->slot, IR_I64, false);
    fn->has_dynalloc = true;
    return;
  }
  case S_EXPR:
    gen_expr(s->expr);
    return;
  case S_DECL:
    gen_local_init(s->var->slot, s->vinit, s->var->ty);
    return;
  case S_BLOCK:
    gen_block(s);
    return;
  case S_IF: {
    Block *t = new_block();
    Block *end = new_block();
    Block *f = s->els ? new_block() : end;
    gen_cond(s->expr, t, f);
    start_block(t);
    gen_stmt(s->then);
    if (s->els) {
      if (!terminated())
        jmp(end);
      start_block(f);
      gen_stmt(s->els);
    }
    start_block(end);
    return;
  }
  case S_WHILE:
  case S_FOR: {
    // The test is placed after the body so that each iteration runs a
    // single conditional branch; the first test is reached by a jump.
    Block *body = new_block();
    Block *inc = new_block();
    Block *test = new_block();
    Block *end = new_block();
    Block *saved_break = break_bb;
    Block *saved_continue = continue_bb;
    if (s->kind == S_FOR && s->init)
      gen_stmt(s->init);
    jmp(test);
    break_bb = end;
    continue_bb = inc;
    start_block(body);
    breakable_depth++;
    loop_depth++;
    gen_stmt(s->then);
    breakable_depth--;
    loop_depth--;
    start_block(inc);
    if (s->inc)
      gen_expr(s->inc);
    start_block(test);
    if (s->expr)
      gen_cond(s->expr, body, end);
    else
      jmp(body);
    start_block(end);
    break_bb = saved_break;
    continue_bb = saved_continue;
    return;
  }
  case S_DO: {
    Block *body = new_block();
    Block *test = new_block();
    Block *end = new_block();
    Block *saved_break = break_bb;
    Block *saved_continue = continue_bb;
    break_bb = end;
    continue_bb = test;
    start_block(body);
    breakable_depth++;
    loop_depth++;
    gen_stmt(s->then);
    breakable_depth--;
    loop_depth--;
    start_block(test);
    gen_cond(s->expr, body, end);
    start_block(end);
    break_bb = saved_break;
    continue_bb = saved_continue;
    return;
  }
  case S_SWITCH:
    gen_switch(s);
    return;
  case S_CASE:
  case S_DEFAULT:
    start_block(s->bb);
    gen_stmt(s->then);
    return;
  case S_BREAK:
    restore_for_exit(false);
    jmp(break_bb);
    start_dead_block();
    return;
  case S_CONTINUE:
    restore_for_exit(true);
    jmp(continue_bb);
    start_dead_block();
    return;
  case S_GOTO:
    jmp(label_block(s->label));
    start_dead_block();
    return;
  case S_LABEL:
    start_block(label_block(s->label));
    gen_stmt(s->then);
    return;
  case S_RETURN:
    gen_return(s);
    start_dead_block();
    return;
  }
  ICE("unknown statement kind %d", s->kind);
}

//
// Function entry: receiving the arguments
//

static Inst *param_reg(int ty, int reg) {
  Inst *p = emit(IR_PARAM, ty, 0);
  p->ival = reg;
  return p;
}

static Inst *arg_area(int64_t off) {
  Inst *a = emit(IR_ARGADDR, IR_I64, 0);
  a->ival = off;
  return a;
}

static void receive_params(void) {
  Type *fty = fsym->ty;
  int ngrn = 0;
  int nsrn = 0;
  int64_t nsaa = 0;

  ArgClass rc = abi_classify(fty->ret);
  if (rc.kind == AC_INDIRECT) {
    sret_slot = alloca_slot(8, 8, NULL);
    store(param_reg(IR_I64, 8), sret_slot, IR_I64, false);
  }

  for (int i = 0; i < fsym->nparams; i++) {
    Sym *var = fsym->params[i];
    Type *ty = var->ty;
    // A definition with an identifier list receives its arguments with
    // the default promotions applied by its callers.
    Type *wire = ty;
    if (fsym->is_knr) {
      if (ty->kind == TY_FLOAT)
        wire = ty_double;
      else if (is_integer(ty) && ty->size < 4)
        wire = ty_int;
    }
    ArgClass c = abi_classify(wire);
    switch (c.kind) {
    case AC_NONE:
      break;
    case AC_INT: {
      Inst *v;
      if (ngrn < 8) {
        v = param_reg(ir_type_of(wire), ngrn++);
      } else {
        int64_t sz = wire->kind == TY_PTR ? 8 : wire->size;
        nsaa = align_up(nsaa, sz);
        v = load_scalar(arg_area(nsaa), wire, false);
        nsaa += sz;
      }
      if (wire != ty)
        v = gen_cast(v, wire, ty);
      store(v, var->slot, ir_memtype_of(ty), false);
      break;
    }
    case AC_FP: {
      Inst *v;
      if (nsrn < 8) {
        v = param_reg(c.elem, nsrn++);
      } else {
        int sz = ir_size(c.elem);
        nsaa = align_up(nsaa, sz);
        v = load(arg_area(nsaa), c.elem, false, false);
        nsaa += sz;
      }
      if (wire != ty)
        v = un(IR_FTRUNC, IR_F32, v);
      store(v, var->slot, ir_memtype_of(ty), false);
      break;
    }
    case AC_INTS:
      if (ngrn + c.n <= 8) {
        // The slot was allocated with room for whole words.
        for (int k = 0; k < c.n; k++)
          store(param_reg(IR_I64, ngrn++), add_off(var->slot, k * 8), IR_I64, false);
      } else {
        ngrn = 8;
        nsaa = align_up(nsaa, 8);
        memcpy_ir(var->slot, arg_area(nsaa), ty->size);
        nsaa += c.n * 8;
      }
      break;
    case AC_HFA: {
      int esz = ir_size(c.elem);
      if (nsrn + c.n <= 8) {
        for (int k = 0; k < c.n; k++)
          store(param_reg(c.elem, nsrn++), add_off(var->slot, k * esz), c.elem, false);
      } else {
        nsrn = 8;
        nsaa = align_up(nsaa, esz);
        memcpy_ir(var->slot, arg_area(nsaa), ty->size);
        nsaa += c.n * esz;
      }
      break;
    }
    case AC_INDIRECT: {
      // The caller passed the address of a copy that belongs to this
      // function: use it in place as the parameter's storage.
      Inst *p;
      if (ngrn < 8) {
        p = param_reg(IR_I64, ngrn++);
      } else {
        nsaa = align_up(nsaa, 8);
        p = load(arg_area(nsaa), IR_I64, false, false);
        nsaa += 8;
      }
      var->slot = p;
      break;
    }
    }
  }
  va_area_off = align_up(nsaa, 8);
}

Func *irgen_func(Sym *s) {
  fsym = s;
  fn = ir_new_func(s);
  fn->no_promote = s->calls_setjmp;
  break_bb = NULL;
  continue_bb = NULL;
  sret_slot = NULL;
  last_alloca = NULL;
  nvla = 0;
  breakable_depth = 0;
  loop_depth = 0;

  entry = new_block();
  ir_append_block(fn, entry);
  cur = entry;

  // One stack slot per automatic object.  Small aggregates that arrive
  // in general registers get a slot rounded up to whole words.
  for (int i = 0; i < s->locals.len; i++) {
    Sym *var = s->locals.data[i];
    int64_t size = var->ty->size;
    int align = var->align;
    if (var->ty->is_vla) {
      var->slot = alloca_slot(8, 8, var->name); // holds the address of the storage
      continue;
    }
    if (var->is_param) {
      ArgClass c = abi_classify(var->ty);
      if (c.kind == AC_INDIRECT) {
        var->slot = NULL;
        continue;
      }
      if (c.kind == AC_INTS) {
        size = c.n * 8;
        if (align < 8)
          align = 8;
      }
    }
    var->slot = alloca_slot(size, align, var->name);
  }

  receive_params();

  // The body starts in its own block so that the entry block (allocas
  // and parameters) never has predecessors.
  Block *body = new_block();
  start_block(body);
  gen_stmt(s->body);
  if (!terminated())
    gen_fallthrough_return();
  ir_compute_preds(fn);
  ir_verify(fn, "irgen");
  return fn;
}
