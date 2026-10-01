// Expressions (C11 6.5): parsing and the type rules.
//
// Every implicit conversion becomes an explicit E_CAST node, so the
// code generator never needs to know C's conversion rules.  Operations
// on constants are folded as they are built; that makes integer
// constant expressions (array sizes, case labels, enumerators) plain
// E_ICONST nodes by the time anything looks at them.

#include "ouro.h"

static Expr *cast_expr(void);
static Expr *unary(void);
static Expr *postfix(Expr *e);

Expr *new_expr(ExprKind k, Type *ty, Token *tok) {
  Expr *e = xalloc(sizeof(Expr));
  e->kind = k;
  e->ty = ty;
  e->tok = tok;
  return e;
}

// Wraps `v` to the width and signedness of `ty`.
static int64_t wrap_int(int64_t v, Type *ty) {
  if (ty->kind == TY_BOOL)
    return v != 0;
  if (ty->kind == TY_PTR)
    return v;
  switch (ty->size) {
  case 1:
    return ty->is_unsigned ? (int64_t)(uint8_t)v : (int64_t)(int8_t)v;
  case 2:
    return ty->is_unsigned ? (int64_t)(uint16_t)v : (int64_t)(int16_t)v;
  case 4:
    return ty->is_unsigned ? (int64_t)(uint32_t)v : (int64_t)(int32_t)v;
  default:
    return v;
  }
}

Expr *new_iconst(int64_t v, Type *ty, Token *tok) {
  Expr *e = new_expr(E_ICONST, ty, tok);
  e->ival = wrap_int(v, ty);
  return e;
}

static Expr *new_fconst(double v, Type *ty, Token *tok) {
  Expr *e = new_expr(E_FCONST, ty, tok);
  e->fval = ty->kind == TY_FLOAT ? (double)(float)v : v;
  return e;
}

static bool is_iconst(Expr *e) {
  return e->kind == E_ICONST;
}

static bool is_fconst(Expr *e) {
  return e->kind == E_FCONST;
}

bool is_lvalue(Expr *e) {
  switch (e->kind) {
  case E_VAR:
    return e->sym->kind == SYM_VAR;
  case E_DEREF:
  case E_STR:
  case E_COMPLIT:
    return true;
  case E_MEMBER:
    return is_lvalue(e->l) || e->l->kind == E_DEREF;
  default:
    return false;
  }
}

static bool is_bitfield(Expr *e) {
  return e->kind == E_MEMBER && e->member->is_bitfield;
}

// Lvalue conversion: arrays and functions decay to pointers; other
// lvalues are read (the type loses its qualifiers).
Expr *rvalue(Expr *e) {
  if (e->ty->kind == TY_ARRAY || e->ty->kind == TY_FUNC) {
    Expr *d = new_expr(E_DECAY, decay(e->ty), e->tok);
    d->l = e;
    return d;
  }
  return e;
}

static bool same_scalar_type(Type *a, Type *b) {
  if (a->kind != b->kind)
    return false;
  if (a->kind == TY_PTR)
    return true;
  if (a->kind == TY_ENUM)
    return a->is_unsigned == b->is_unsigned && a->size == b->size;
  return true;
}

// Converts `e` to `ty` (C11 6.3), folding constants.
Expr *cast_to(Expr *e, Type *ty) {
  ty = unqual(ty);
  if (ty->kind == TY_VOID) {
    if (e->ty->kind == TY_VOID)
      return e;
    Expr *c = new_expr(E_CAST, ty, e->tok);
    c->l = e;
    return c;
  }
  e = rvalue(e);
  Type *from = e->ty;
  if (is_record(ty) || is_record(from)) {
    if (is_record(ty) && is_record(from) && ty->rec == from->rec)
      return e;
    error_tok(e->tok, "cannot convert '%s' to '%s'", type_str(from), type_str(ty));
  }
  if (from->kind == TY_VOID)
    error_tok(e->tok, "expression of type 'void' cannot be used as a value");
  if (ty->kind == TY_ARRAY || ty->kind == TY_FUNC)
    error_tok(e->tok, "cast to '%s' is not allowed", type_str(ty));
  if ((is_flonum(ty) && is_ptr(from)) || (is_ptr(ty) && is_flonum(from)))
    error_tok(e->tok, "cannot convert '%s' to '%s'", type_str(from), type_str(ty));
  if (same_scalar_type(from, ty)) {
    if (e->ty == ty)
      return e;
    Expr *c = new_expr(E_CAST, ty, e->tok);
    c->l = e;
    if (is_iconst(e))
      return new_iconst(e->ival, ty, e->tok);
    if (is_fconst(e))
      return new_fconst(e->fval, ty, e->tok);
    return c;
  }

  if (is_iconst(e)) {
    if (is_flonum(ty))
      return new_fconst(from->is_unsigned ? (double)(uint64_t)e->ival : (double)e->ival, ty,
                        e->tok);
    return new_iconst(e->ival, ty, e->tok);
  }
  if (is_fconst(e)) {
    if (is_flonum(ty))
      return new_fconst(e->fval, ty, e->tok);
    if (ty->kind == TY_BOOL)
      return new_iconst(e->fval != 0, ty, e->tok);
    // Out-of-range conversions are undefined; fold only what is defined.
    double v = e->fval;
    if (ty->is_unsigned ? (v > -1.0 && v < 18446744073709551616.0)
                        : (v > -9223372036854775809.0 && v < 9223372036854775808.0)) {
      int64_t iv = ty->is_unsigned && v >= 9223372036854775808.0
                       ? (int64_t)(uint64_t)v
                       : (int64_t)v;
      return new_iconst(iv, ty, e->tok);
    }
  }
  Expr *c = new_expr(E_CAST, ty, e->tok);
  c->l = e;
  return c;
}

bool is_null_ptr_const(Expr *e) {
  // An integer constant expression with value 0, or such an
  // expression cast to void *.
  if (e->kind == E_ICONST && e->ival == 0 && (is_integer(e->ty) || (is_ptr(e->ty) && e->ty->base->kind == TY_VOID)))
    return true;
  if (e->kind == E_CAST && is_ptr(e->ty) && e->ty->base->kind == TY_VOID)
    return is_null_ptr_const(e->l);
  return false;
}

// Integer promotion of an expression; bit-fields narrower than int
// promote to int even when unsigned (C11 6.3.1.1p2).
static Type *promoted_type(Expr *e) {
  if (is_bitfield(e) && e->member->bit_width < 32 && is_integer(e->ty) &&
      e->ty->kind != TY_BOOL && int_rank(e->ty) <= int_rank(ty_int))
    return ty_int;
  if (is_bitfield(e) && e->member->bit_width < 32 && e->ty->size == 8)
    return ty_int;
  return promote_int(e->ty);
}

static Expr *promote(Expr *e) {
  e = rvalue(e);
  if (!is_integer(e->ty))
    return e;
  return cast_to(e, promoted_type(e));
}

static Type *common_type_of(Expr *l, Expr *r) {
  if (is_flonum(l->ty) || is_flonum(r->ty))
    return arith_conv(l->ty, r->ty);
  return arith_conv(promoted_type(l), promoted_type(r));
}

static void usual_arith(Expr **l, Expr **r) {
  Type *t = common_type_of(*l, *r);
  *l = cast_to(*l, t);
  *r = cast_to(*r, t);
}

//
// Constant folding
//

static bool fold_int_binary(ExprKind k, Type *ty, int64_t a, int64_t b, int64_t *out) {
  uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
  bool u = ty->is_unsigned;
  int bits = (int)ty->size * 8;
  switch (k) {
  case E_ADD:
    *out = (int64_t)(ua + ub);
    break;
  case E_SUB:
    *out = (int64_t)(ua - ub);
    break;
  case E_MUL:
    *out = (int64_t)(ua * ub);
    break;
  case E_DIV:
  case E_MOD:
    if (b == 0)
      return false;
    if (u) {
      if (bits == 32) {
        ua = (uint32_t)ua;
        ub = (uint32_t)ub;
      }
      *out = (int64_t)(k == E_DIV ? ua / ub : ua % ub);
    } else {
      if (b == -1 && a == (bits == 32 ? INT32_MIN : INT64_MIN))
        return false; // overflow: undefined
      *out = k == E_DIV ? a / b : a % b;
    }
    break;
  case E_SHL:
    if (b < 0 || b >= bits)
      return false;
    *out = (int64_t)(ua << b);
    break;
  case E_SHR:
    if (b < 0 || b >= bits)
      return false;
    if (u)
      *out = (int64_t)((bits == 32 ? (uint32_t)ua : ua) >> b);
    else
      *out = a >> b;
    break;
  case E_BITAND:
    *out = a & b;
    break;
  case E_BITOR:
    *out = a | b;
    break;
  case E_BITXOR:
    *out = a ^ b;
    break;
  default:
    return false;
  }
  return true;
}

static int64_t fold_compare(ExprKind k, Type *ty, int64_t a, int64_t b) {
  bool u = ty->is_unsigned || ty->kind == TY_PTR;
  uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
  switch (k) {
  case E_EQ:
    return a == b;
  case E_NE:
    return a != b;
  case E_LT:
    return u ? ua < ub : a < b;
  case E_LE:
    return u ? ua <= ub : a <= b;
  case E_GT:
    return u ? ua > ub : a > b;
  case E_GE:
    return u ? ua >= ub : a >= b;
  default:
    ICE("bad comparison");
  }
}

static int64_t fold_fcompare(ExprKind k, double a, double b) {
  switch (k) {
  case E_EQ:
    return a == b;
  case E_NE:
    return a != b;
  case E_LT:
    return a < b;
  case E_LE:
    return a <= b;
  case E_GT:
    return a > b;
  case E_GE:
    return a >= b;
  default:
    ICE("bad comparison");
  }
}

static Expr *binop(ExprKind k, Type *ty, Expr *l, Expr *r, Token *tok) {
  if (is_iconst(l) && is_iconst(r) && is_integer(ty)) {
    int64_t v;
    if (fold_int_binary(k, ty, l->ival, r->ival, &v))
      return new_iconst(v, ty, tok);
  }
  if (is_fconst(l) && is_fconst(r) && is_flonum(ty)) {
    double a = l->fval, b = r->fval, v;
    switch (k) {
    case E_ADD:
      v = a + b;
      break;
    case E_SUB:
      v = a - b;
      break;
    case E_MUL:
      v = a * b;
      break;
    case E_DIV:
      v = a / b;
      break;
    default:
      goto no_fold;
    }
    if (ty->kind == TY_FLOAT)
      v = (float)v;
    return new_fconst(v, ty, tok);
  }
no_fold:;
  Expr *e = new_expr(k, ty, tok);
  e->l = l;
  e->r = r;
  return e;
}

//
// Integer constant evaluation and address constants
//

bool eval_int_const(Expr *e, int64_t *out) {
  switch (e->kind) {
  case E_ICONST:
    if (!is_integer(e->ty) && !is_ptr(e->ty))
      return false;
    *out = e->ival;
    return true;
  case E_CAST: {
    if (is_integer(e->ty) && is_flonum(e->l->ty) && e->l->kind == E_FCONST) {
      *out = wrap_int((int64_t)e->l->fval, e->ty);
      return true;
    }
    if (!is_integer(e->ty) && !is_ptr(e->ty))
      return false;
    int64_t v;
    if (eval_int_const(e->l, &v)) {
      *out = wrap_int(v, e->ty);
      return true;
    }
    // offsetof written as (size_t)&((T *)0)->member
    Sym *sym;
    int64_t addend;
    if (is_integer(e->ty) && eval_static_addr(e->l, &sym, &addend) && !sym) {
      *out = wrap_int(addend, e->ty);
      return true;
    }
    return false;
  }
  case E_COND: {
    int64_t c, v;
    if (!eval_int_const(e->cond, &c))
      return false;
    if (!eval_int_const(c ? e->l : e->r, &v))
      return false;
    *out = v;
    return true;
  }
  case E_NEG:
  case E_BITNOT:
  case E_NOT: {
    int64_t v;
    if (!eval_int_const(e->l, &v))
      return false;
    *out = wrap_int(e->kind == E_NEG ? (int64_t)(0 - (uint64_t)v) : e->kind == E_BITNOT ? ~v : !v,
                    e->ty);
    return true;
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
    int64_t a, b, v;
    if (!is_integer(e->ty) || !eval_int_const(e->l, &a) || !eval_int_const(e->r, &b))
      return false;
    if (!fold_int_binary(e->kind, e->ty, a, b, &v))
      return false;
    *out = wrap_int(v, e->ty);
    return true;
  }
  case E_EQ:
  case E_NE:
  case E_LT:
  case E_LE:
  case E_GT:
  case E_GE: {
    int64_t a, b;
    if (!is_integer(e->l->ty) || !eval_int_const(e->l, &a) || !eval_int_const(e->r, &b))
      return false;
    *out = fold_compare(e->kind, e->l->ty, a, b);
    return true;
  }
  case E_LOGAND:
  case E_LOGOR: {
    int64_t a, b;
    if (!eval_int_const(e->l, &a))
      return false;
    if (e->kind == E_LOGAND && !a) {
      *out = 0;
      return true;
    }
    if (e->kind == E_LOGOR && a) {
      *out = 1;
      return true;
    }
    if (!eval_int_const(e->r, &b))
      return false;
    *out = b != 0;
    return true;
  }
  default:
    return false;
  }
}

bool eval_float_const(Expr *e, double *out) {
  if (e->kind == E_FCONST) {
    *out = e->fval;
    return true;
  }
  if (e->kind == E_ICONST) {
    *out = e->ty->is_unsigned ? (double)(uint64_t)e->ival : (double)e->ival;
    return true;
  }
  if (e->kind == E_CAST && is_arith(e->l->ty)) {
    double v;
    if (!eval_float_const(e->l, &v))
      return false;
    *out = e->ty->kind == TY_FLOAT ? (double)(float)v : v;
    return true;
  }
  return false;
}

// Recognises an address constant (C11 6.6p9): the address of a static
// object or function plus a constant offset.  A null `sym` means an
// absolute address (an integer cast to a pointer).
bool eval_static_addr(Expr *e, Sym **sym, int64_t *addend) {
  switch (e->kind) {
  case E_ICONST:
    *sym = NULL;
    *addend = e->ival;
    return true;
  case E_VAR:
    // Only reached through & or array/function decay.
    if (e->sym->kind == SYM_FUNC || (e->sym->is_global && e->sym->kind == SYM_VAR)) {
      *sym = e->sym;
      *addend = 0;
      return true;
    }
    return false;
  case E_STR:
  case E_COMPLIT:
    if (e->sym && e->sym->is_global) {
      *sym = e->sym;
      *addend = 0;
      return true;
    }
    return false;
  case E_ADDR:
  case E_DECAY:
    return eval_static_addr(e->l, sym, addend);
  case E_DEREF:
    // &*p is p; &p->m and p[i] are handled by E_MEMBER and E_PTRADD.
    return eval_static_addr(e->l, sym, addend);
  case E_MEMBER:
    if (!eval_static_addr(e->l, sym, addend))
      return false;
    *addend += e->member->offset;
    return true;
  case E_CAST:
    if (!is_ptr(e->ty) && !(is_integer(e->ty) && e->ty->size == 8))
      return false;
    return eval_static_addr(e->l, sym, addend);
  case E_PTRADD: {
    int64_t idx;
    if (!eval_int_const(e->r, &idx))
      return false;
    if (!eval_static_addr(e->l, sym, addend))
      return false;
    *addend += idx * e->scale;
    return true;
  }
  case E_COND: {
    int64_t c;
    if (!eval_int_const(e->cond, &c))
      return false;
    return eval_static_addr(c ? e->l : e->r, sym, addend);
  }
  default:
    return false;
  }
}

int64_t parse_const_int(void) {
  Token *t = peek();
  Expr *e = parse_cond_expr();
  int64_t v;
  if (!is_integer(e->ty) || !eval_int_const(e, &v))
    error_tok(t, "expression is not an integer constant expression");
  return v;
}

//
// Checks shared by several operators
//

static void check_modifiable(Expr *e, Token *op) {
  if (!is_lvalue(e))
    error_tok(e->tok, "expression is not assignable");
  if (e->ty->kind == TY_ARRAY)
    error_tok(e->tok, "array type '%s' is not assignable", type_str(e->ty));
  if (e->ty->qual & Q_CONST) {
    if (e->kind == E_VAR)
      error_tok(op, "cannot assign to variable '%s' with const-qualified type '%s'",
                e->sym->name->name, type_str(e->ty));
    error_tok(op, "cannot assign to lvalue with const-qualified type '%s'", type_str(e->ty));
  }
  if (!is_complete(e->ty))
    error_tok(e->tok, "incomplete type '%s' is not assignable", type_str(e->ty));
}

static bool pointee_compatible(Type *a, Type *b) {
  return types_compatible(unqual(a->base), unqual(b->base));
}

// Conversion as if by assignment (C11 6.5.16.1).
Expr *assign_conv(Expr *e, Type *ty, Token *tok, const char *what) {
  e = rvalue(e);
  Type *from = e->ty;
  ty = unqual(ty);
  if (is_arith(ty) && is_arith(from))
    return cast_to(e, ty);
  if (is_record(ty)) {
    if (is_record(from) && from->rec == ty->rec)
      return e;
    error_tok(tok, "%s '%s' to parameter of incompatible type '%s'", what, type_str(from),
              type_str(ty));
  }
  if (ty->kind == TY_BOOL && is_ptr(from))
    return cast_to(e, ty);
  if (is_ptr(ty)) {
    if (is_null_ptr_const(e))
      return cast_to(e, ty);
    if (is_ptr(from)) {
      Type *tb = ty->base, *fb = from->base;
      bool void_ok = tb->kind == TY_VOID || fb->kind == TY_VOID;
      if (!void_ok && !pointee_compatible(ty, from)) {
        // char * and unsigned char * etc. differ only in signedness: clang
        // reports these separately; one warning covers both here.
        warn_tok(tok, "incompatible pointer types %s '%s' to '%s'", what, type_str(from),
                 type_str(ty));
      } else if ((fb->qual & ~tb->qual) & (Q_CONST | Q_VOLATILE)) {
        warn_tok(tok, "%s '%s' to '%s' discards qualifiers", what, type_str(from), type_str(ty));
      }
      return cast_to(e, ty);
    }
    if (is_integer(from)) {
      warn_tok(tok, "incompatible integer to pointer conversion %s '%s' to '%s'", what,
               type_str(from), type_str(ty));
      return cast_to(e, ty);
    }
  }
  if (is_integer(ty) && is_ptr(from)) {
    warn_tok(tok, "incompatible pointer to integer conversion %s '%s' to '%s'", what,
             type_str(from), type_str(ty));
    return cast_to(e, ty);
  }
  error_tok(tok, "%s '%s' to incompatible type '%s'", what, type_str(from), type_str(ty));
}

//
// Operators
//

static Expr *ptr_add(Expr *p, Expr *idx, Token *tok, bool negate) {
  Type *base = p->ty->base;
  if (!is_complete(base) && base->kind != TY_VOID)
    error_tok(tok, "arithmetic on a pointer to an incomplete type '%s'", type_str(base));
  if (base->kind == TY_FUNC)
    error_tok(tok, "arithmetic on a pointer to the function type '%s'", type_str(base));
  idx = cast_to(idx, ty_long);
  if (negate)
    idx = binop(E_SUB, ty_long, new_iconst(0, ty_long, tok), idx, tok);
  Expr *e = new_expr(E_PTRADD, p->ty, tok);
  e->l = p;
  e->r = idx;
  e->scale = base->kind == TY_VOID ? 1 : base->size;
  if (p->kind == E_ICONST && idx->kind == E_ICONST)
    return new_iconst(p->ival + idx->ival * e->scale, p->ty, tok);
  return e;
}

static Expr *add_expr(Expr *l, Expr *r, Token *tok) {
  l = rvalue(l);
  r = rvalue(r);
  if (is_arith(l->ty) && is_arith(r->ty)) {
    usual_arith(&l, &r);
    return binop(E_ADD, l->ty, l, r, tok);
  }
  if (is_ptr(l->ty) && is_integer(r->ty))
    return ptr_add(l, r, tok, false);
  if (is_integer(l->ty) && is_ptr(r->ty))
    return ptr_add(r, l, tok, false);
  error_tok(tok, "invalid operands to binary expression ('%s' and '%s')", type_str(l->ty),
            type_str(r->ty));
}

static Expr *sub_expr(Expr *l, Expr *r, Token *tok) {
  l = rvalue(l);
  r = rvalue(r);
  if (is_arith(l->ty) && is_arith(r->ty)) {
    usual_arith(&l, &r);
    return binop(E_SUB, l->ty, l, r, tok);
  }
  if (is_ptr(l->ty) && is_integer(r->ty))
    return ptr_add(l, r, tok, true);
  if (is_ptr(l->ty) && is_ptr(r->ty)) {
    if (!pointee_compatible(l->ty, r->ty))
      error_tok(tok, "'%s' and '%s' are not pointers to compatible types", type_str(l->ty),
                type_str(r->ty));
    Type *base = l->ty->base;
    Expr *e = new_expr(E_PTRDIFF, ty_long, tok);
    e->l = l;
    e->r = r;
    e->scale = base->kind == TY_VOID ? 1 : base->size;
    if (e->scale <= 0)
      error_tok(tok, "arithmetic on pointers to an incomplete type");
    return e;
  }
  error_tok(tok, "invalid operands to binary expression ('%s' and '%s')", type_str(l->ty),
            type_str(r->ty));
}

static Expr *arith_expr(ExprKind k, Expr *l, Expr *r, Token *tok) {
  l = rvalue(l);
  r = rvalue(r);
  bool int_only = k == E_MOD || k == E_BITAND || k == E_BITOR || k == E_BITXOR;
  if (int_only ? !(is_integer(l->ty) && is_integer(r->ty))
               : !(is_arith(l->ty) && is_arith(r->ty)))
    error_tok(tok, "invalid operands to binary expression ('%s' and '%s')", type_str(l->ty),
              type_str(r->ty));
  usual_arith(&l, &r);
  if ((k == E_DIV || k == E_MOD) && is_iconst(r) && r->ival == 0 && is_integer(r->ty))
    warn_tok(tok, "division by zero is undefined");
  return binop(k, l->ty, l, r, tok);
}

static Expr *shift_expr(ExprKind k, Expr *l, Expr *r, Token *tok) {
  l = rvalue(l);
  r = rvalue(r);
  if (!is_integer(l->ty) || !is_integer(r->ty))
    error_tok(tok, "invalid operands to binary expression ('%s' and '%s')", type_str(l->ty),
              type_str(r->ty));
  l = promote(l);
  r = promote(r);
  if (is_iconst(r) && (r->ival < 0 || r->ival >= l->ty->size * 8))
    warn_tok(tok, "shift count is negative or >= the width of the type");
  return binop(k, l->ty, l, cast_to(r, l->ty), tok);
}

static Expr *compare_expr(ExprKind k, Expr *l, Expr *r, Token *tok) {
  l = rvalue(l);
  r = rvalue(r);
  if (is_arith(l->ty) && is_arith(r->ty)) {
    usual_arith(&l, &r);
    if (is_iconst(l) && is_iconst(r))
      return new_iconst(fold_compare(k, l->ty, l->ival, r->ival), ty_int, tok);
    if (is_fconst(l) && is_fconst(r))
      return new_iconst(fold_fcompare(k, l->fval, r->fval), ty_int, tok);
  } else if (is_ptr(l->ty) && is_ptr(r->ty)) {
    if (!pointee_compatible(l->ty, r->ty) && l->ty->base->kind != TY_VOID &&
        r->ty->base->kind != TY_VOID)
      warn_tok(tok, "comparison of distinct pointer types ('%s' and '%s')", type_str(l->ty),
               type_str(r->ty));
  } else if (is_ptr(l->ty) && is_integer(r->ty)) {
    if (!is_null_ptr_const(r) || (k != E_EQ && k != E_NE))
      warn_tok(tok, "comparison between pointer and integer ('%s' and '%s')", type_str(l->ty),
               type_str(r->ty));
    r = cast_to(r, l->ty);
  } else if (is_integer(l->ty) && is_ptr(r->ty)) {
    if (!is_null_ptr_const(l) || (k != E_EQ && k != E_NE))
      warn_tok(tok, "comparison between pointer and integer ('%s' and '%s')", type_str(l->ty),
               type_str(r->ty));
    l = cast_to(l, r->ty);
  } else {
    error_tok(tok, "invalid operands to binary expression ('%s' and '%s')", type_str(l->ty),
              type_str(r->ty));
  }
  Expr *e = new_expr(k, ty_int, tok);
  e->l = l;
  e->r = r;
  return e;
}

// Converts a scalar to the condition of && || ! ?: (compared with 0).
Expr *cond_expr(Expr *e) {
  e = rvalue(e);
  if (!is_scalar(e->ty))
    error_tok(e->tok, "expression of type '%s' is not a scalar", type_str(e->ty));
  return e;
}

static bool const_truth(Expr *e, bool *v) {
  if (is_iconst(e)) {
    *v = e->ival != 0;
    return true;
  }
  if (is_fconst(e)) {
    *v = e->fval != 0;
    return true;
  }
  return false;
}

static Expr *logical(ExprKind k, Expr *l, Expr *r, Token *tok) {
  l = cond_expr(l);
  r = cond_expr(r);
  bool lv, rv;
  if (const_truth(l, &lv)) {
    if (k == E_LOGAND && !lv)
      return new_iconst(0, ty_int, tok);
    if (k == E_LOGOR && lv)
      return new_iconst(1, ty_int, tok);
    if (const_truth(r, &rv))
      return new_iconst(rv, ty_int, tok);
  }
  Expr *e = new_expr(k, ty_int, tok);
  e->l = l;
  e->r = r;
  return e;
}

static Expr *conditional(Expr *c, Expr *a, Expr *b, Token *tok) {
  c = cond_expr(c);
  a = rvalue(a);
  b = rvalue(b);
  Type *ty;
  if (is_arith(a->ty) && is_arith(b->ty)) {
    usual_arith(&a, &b);
    ty = a->ty;
  } else if (is_record(a->ty) && is_record(b->ty) && a->ty->rec == b->ty->rec) {
    ty = unqual(a->ty);
  } else if (a->ty->kind == TY_VOID && b->ty->kind == TY_VOID) {
    ty = ty_void;
  } else if (is_ptr(a->ty) && is_ptr(b->ty)) {
    int q = a->ty->base->qual | b->ty->base->qual;
    if (a->ty->base->kind == TY_VOID || b->ty->base->kind == TY_VOID) {
      // a null pointer constant takes the other side's type
      if (is_null_ptr_const(a))
        ty = b->ty;
      else if (is_null_ptr_const(b))
        ty = a->ty;
      else
        ty = pointer_to(qualified(ty_void, q));
    } else if (pointee_compatible(a->ty, b->ty)) {
      ty = pointer_to(qualified(unqual(a->ty->base), q));
    } else {
      warn_tok(tok, "pointer type mismatch ('%s' and '%s')", type_str(a->ty), type_str(b->ty));
      ty = pointer_to(qualified(ty_void, q));
    }
    a = cast_to(a, ty);
    b = cast_to(b, ty);
  } else if (is_ptr(a->ty) && is_integer(b->ty)) {
    if (!is_null_ptr_const(b))
      warn_tok(tok, "pointer/integer type mismatch in conditional expression ('%s' and '%s')",
               type_str(a->ty), type_str(b->ty));
    ty = a->ty;
    b = cast_to(b, ty);
  } else if (is_integer(a->ty) && is_ptr(b->ty)) {
    if (!is_null_ptr_const(a))
      warn_tok(tok, "pointer/integer type mismatch in conditional expression ('%s' and '%s')",
               type_str(a->ty), type_str(b->ty));
    ty = b->ty;
    a = cast_to(a, ty);
  } else if (a->ty->kind == TY_VOID || b->ty->kind == TY_VOID) {
    ty = ty_void;
    a = cast_to(a, ty_void);
    b = cast_to(b, ty_void);
  } else {
    error_tok(tok, "incompatible operand types ('%s' and '%s')", type_str(a->ty),
              type_str(b->ty));
  }
  bool cv;
  if (const_truth(c, &cv) && (is_iconst(cv ? a : b) || is_fconst(cv ? a : b)))
    return cv ? a : b;
  Expr *e = new_expr(E_COND, ty, tok);
  e->cond = c;
  e->l = a;
  e->r = b;
  return e;
}

static Expr *assign_expr(Expr *l, Expr *r, Token *tok) {
  check_modifiable(l, tok);
  Expr *e = new_expr(E_ASSIGN, unqual(l->ty), tok);
  e->l = l;
  e->r = assign_conv(r, l->ty, tok, "assigning");
  return e;
}

static Expr *op_assign(ExprKind op, Expr *l, Expr *r, Token *tok) {
  check_modifiable(l, tok);
  r = rvalue(r);
  Expr *e = new_expr(E_OPASSIGN, unqual(l->ty), tok);
  e->l = l;
  e->op = op;
  if ((op == E_ADD || op == E_SUB) && is_ptr(l->ty)) {
    if (!is_integer(r->ty))
      error_tok(tok, "invalid operands to binary expression ('%s' and '%s')", type_str(l->ty),
                type_str(r->ty));
    Type *base = l->ty->base;
    if (!is_complete(base) && base->kind != TY_VOID)
      error_tok(tok, "arithmetic on a pointer to an incomplete type '%s'", type_str(base));
    e->op = E_PTRADD;
    e->cty = l->ty;
    e->scale = base->kind == TY_VOID ? 1 : base->size;
    r = cast_to(r, ty_long);
    if (op == E_SUB)
      r = binop(E_SUB, ty_long, new_iconst(0, ty_long, tok), r, tok);
    e->r = r;
    return e;
  }
  if (op == E_SHL || op == E_SHR) {
    if (!is_integer(l->ty) || !is_integer(r->ty))
      error_tok(tok, "invalid operands to binary expression ('%s' and '%s')", type_str(l->ty),
                type_str(r->ty));
    e->cty = promoted_type(l);
    e->r = cast_to(promote(r), e->cty);
    return e;
  }
  bool int_only = op == E_MOD || op == E_BITAND || op == E_BITOR || op == E_BITXOR;
  if (int_only ? !(is_integer(l->ty) && is_integer(r->ty))
               : !(is_arith(l->ty) && is_arith(r->ty)))
    error_tok(tok, "invalid operands to binary expression ('%s' and '%s')", type_str(l->ty),
              type_str(r->ty));
  e->cty = common_type_of(l, r);
  e->r = cast_to(r, e->cty);
  return e;
}

static Expr *incdec(Expr *l, int delta, bool post, Token *tok) {
  check_modifiable(l, tok);
  if (!is_scalar(l->ty))
    error_tok(tok, "cannot increment value of type '%s'", type_str(l->ty));
  Expr *e = new_expr(post ? E_POSTINC : E_PREINC, unqual(l->ty), tok);
  e->l = l;
  e->ival = delta;
  if (is_ptr(l->ty)) {
    Type *base = l->ty->base;
    if (!is_complete(base) && base->kind != TY_VOID)
      error_tok(tok, "arithmetic on a pointer to an incomplete type '%s'", type_str(base));
    e->scale = base->kind == TY_VOID ? 1 : base->size;
  }
  return e;
}

static Expr *deref(Expr *e, Token *tok) {
  e = rvalue(e);
  if (!is_ptr(e->ty))
    error_tok(tok, "indirection requires pointer operand ('%s' invalid)", type_str(e->ty));
  Expr *d = new_expr(E_DEREF, e->ty->base, tok);
  d->l = e;
  return d;
}

static Expr *address_of(Expr *e, Token *tok) {
  if (e->ty->kind == TY_FUNC) {
    Expr *a = new_expr(E_ADDR, pointer_to(e->ty), tok);
    a->l = e;
    return a;
  }
  if (!is_lvalue(e) && !(e->kind == E_MEMBER))
    error_tok(tok, "cannot take the address of an rvalue of type '%s'", type_str(e->ty));
  if (is_bitfield(e))
    error_tok(tok, "address of bit-field requested");
  if (e->kind == E_VAR && e->sym->sc == SC_REGISTER)
    error_tok(tok, "address of register variable requested");
  if (e->kind == E_VAR)
    e->sym->is_addr_taken = true;
  Expr *a = new_expr(E_ADDR, pointer_to(e->ty), tok);
  a->l = e;
  return a;
}

static Expr *member_access(Expr *base, Token *name, Token *tok) {
  Type *ty = base->ty;
  if (!is_record(ty))
    error_tok(tok, "member reference base type '%s' is not a structure or union", type_str(ty));
  if (!ty->rec->complete)
    error_tok(tok, "incomplete definition of type '%s'", type_str(ty));
  Vec path = {0};
  Member *m = find_member(ty, name->ident, &path);
  if (!m)
    error_tok(name, "no member named '%s' in '%s'", name->ident->name, type_str(ty));
  Expr *e = base;
  int q = ty->qual;
  for (int i = 0; i <= path.len; i++) {
    Member *step = i < path.len ? path.data[i] : m;
    Expr *me = new_expr(E_MEMBER, qualified(step->ty, q), name);
    me->l = e;
    me->member = step;
    e = me;
    q |= step->ty->qual;
  }
  return e;
}

//
// Function calls
//

static Expr *default_promote(Expr *e) {
  e = rvalue(e);
  if (e->ty->kind == TY_FLOAT)
    return cast_to(e, ty_double);
  if (is_integer(e->ty))
    return promote(e);
  return e;
}

static Expr *call(Expr *fn, Token *tok) {
  fn = rvalue(fn);
  if (!is_ptr(fn->ty) || fn->ty->base->kind != TY_FUNC)
    error_tok(tok, "called object type '%s' is not a function or function pointer",
              type_str(fn->ty));
  Type *ft = fn->ty->base;
  Vec args = {0};
  Token *rparen = peek();
  if (!accept(')')) {
    for (;;) {
      vec_push(&args, parse_assign());
      rparen = peek();
      if (accept(')'))
        break;
      expect(',', "between arguments");
    }
  }
  if (ft->has_proto) {
    if (args.len < ft->nparams)
      error_tok(rparen, "too few arguments to function call, expected %d, have %d", ft->nparams,
                args.len);
    if (args.len > ft->nparams && !ft->variadic)
      error_tok(((Expr *)args.data[ft->nparams])->tok,
                "too many arguments to function call, expected %d, have %d", ft->nparams,
                args.len);
  }
  for (int i = 0; i < args.len; i++) {
    Expr *a = args.data[i];
    if (ft->has_proto && i < ft->nparams) {
      args.data[i] = assign_conv(a, ft->params[i], a->tok, "passing");
    } else {
      a = default_promote(a);
      if (a->ty->kind == TY_VOID)
        error_tok(a->tok, "argument may not have 'void' type");
      args.data[i] = a;
    }
    Expr *ai = args.data[i];
    if (is_record(ai->ty) && !is_complete(ai->ty))
      error_tok(ai->tok, "argument type '%s' is incomplete", type_str(ai->ty));
  }
  if (!is_complete(ft->ret) && ft->ret->kind != TY_VOID)
    error_tok(tok, "calling function with incomplete return type '%s'", type_str(ft->ret));

  Expr *e = new_expr(E_CALL, unqual(ft->ret), tok);
  e->l = fn;
  e->args = (Expr **)args.data;
  e->nargs = args.len;

  // Calls to setjmp return twice; the caller must keep its variables in
  // memory (see irgen.c).
  Expr *callee = fn->kind == E_DECAY ? fn->l : fn;
  if (callee->kind == E_VAR && current_function()) {
    char *n = callee->sym->name->name;
    if (str_eq(n, "setjmp") || str_eq(n, "_setjmp") || str_eq(n, "sigsetjmp") ||
        str_eq(n, "__sigsetjmp"))
      current_function()->calls_setjmp = true;
  }
  return e;
}

//
// Builtins that look like functions
//

static Expr *builtin(Token *t) {
  char *n = t->ident->name;
  if (str_eq(n, "__builtin_va_start")) {
    expect('(', "after __builtin_va_start");
    Expr *ap = parse_assign();
    expect(',', "in __builtin_va_start");
    parse_assign();
    expect(')', "after __builtin_va_start");
    Sym *fn = current_function();
    if (!fn || !fn->ty->variadic)
      error_tok(t, "'va_start' used in function with fixed arguments");
    fn->has_va_start = true;
    Expr *e = new_expr(E_VA_START, ty_void, t);
    e->l = ap;
    return e;
  }
  if (str_eq(n, "__builtin_va_end")) {
    expect('(', "after __builtin_va_end");
    Expr *ap = parse_assign();
    expect(')', "after __builtin_va_end");
    return cast_to(ap, ty_void);
  }
  if (str_eq(n, "__builtin_va_arg")) {
    expect('(', "after __builtin_va_arg");
    Expr *ap = parse_assign();
    expect(',', "in __builtin_va_arg");
    Type *ty = parse_typename();
    expect(')', "after __builtin_va_arg");
    if (!is_lvalue(ap))
      error_tok(t, "va_arg requires a va_list lvalue");
    Expr *e = new_expr(E_VA_ARG, unqual(ty), t);
    e->l = ap;
    return e;
  }
  if (str_eq(n, "__builtin_va_copy")) {
    expect('(', "after __builtin_va_copy");
    Expr *d = parse_assign();
    expect(',', "in __builtin_va_copy");
    Expr *s = parse_assign();
    expect(')', "after __builtin_va_copy");
    return cast_to(assign_expr(d, s, t), ty_void);
  }
  if (str_eq(n, "__builtin_offsetof")) {
    expect('(', "after __builtin_offsetof");
    Type *ty = parse_typename();
    expect(',', "in __builtin_offsetof");
    if (!is_record(ty))
      error_tok(t, "offsetof requires a struct or union type");
    // Build &((ty *)0)->designator and evaluate it as an address constant.
    Expr *base = deref(new_iconst(0, pointer_to(ty), t), t);
    Token *mt = peek();
    Ident *id = NULL;
    if (mt->kind == TK_IDENT)
      id = mt->ident;
    if (!id)
      error_tok(mt, "expected a member name");
    next_tok();
    Expr *e = member_access(base, mt, mt);
    for (;;) {
      if (accept('.')) {
        Token *nt = expect(TK_IDENT, "after '.'");
        e = member_access(e, nt, nt);
      } else if (peek()->kind == '[') {
        Token *bt = next_tok();
        Expr *idx = parse_expr();
        expect(']', "in offsetof designator");
        e = deref(add_expr(e, idx, bt), bt);
      } else {
        break;
      }
    }
    expect(')', "after __builtin_offsetof");
    Sym *sym;
    int64_t off;
    if (!eval_static_addr(address_of(e, t), &sym, &off) || sym)
      error_tok(t, "offsetof designator is not a constant");
    return new_iconst(off, ty_ulong, t);
  }
  if (str_eq(n, "__builtin_expect")) {
    expect('(', "after __builtin_expect");
    Expr *e = parse_assign();
    expect(',', "in __builtin_expect");
    parse_assign();
    expect(')', "after __builtin_expect");
    return cast_to(e, ty_long);
  }
  if (str_eq(n, "__builtin_trap") || str_eq(n, "__builtin_unreachable") ||
      str_eq(n, "__builtin_abort")) {
    expect('(', "after builtin");
    expect(')', "after builtin");
    return new_expr(E_BUILTIN_TRAP, ty_void, t);
  }
  if (str_eq(n, "__builtin_huge_val") || str_eq(n, "__builtin_inf") ||
      str_eq(n, "__builtin_huge_valf") || str_eq(n, "__builtin_inff")) {
    expect('(', "after builtin");
    expect(')', "after builtin");
    bool f = n[strlen(n) - 1] == 'f';
    double inf = 1e308 * 10;
    return new_fconst(inf, f ? ty_float : ty_double, t);
  }
  if (str_eq(n, "__builtin_nan") || str_eq(n, "__builtin_nanf")) {
    expect('(', "after builtin");
    parse_assign();
    expect(')', "after builtin");
    bool f = n[strlen(n) - 1] == 'f';
    double zero = 0.0;
    return new_fconst(zero / zero, f ? ty_float : ty_double, t);
  }
  if (str_eq(n, "__builtin_constant_p")) {
    expect('(', "after __builtin_constant_p");
    Expr *e = parse_assign();
    expect(')', "after __builtin_constant_p");
    return new_iconst(is_iconst(e) || is_fconst(e), ty_int, t);
  }
  return NULL;
}

//
// Primary expressions
//

static Expr *generic_selection(Token *t) {
  expect('(', "after _Generic");
  Expr *ctrl = rvalue(parse_assign());
  Type *cty = unqual(ctrl->ty);
  Expr *chosen = NULL;
  Expr *dflt = NULL;
  while (accept(',')) {
    if (accept(KW_DEFAULT)) {
      expect(':', "after default");
      Expr *e = parse_assign();
      if (dflt)
        error_tok(t, "duplicate default generic association");
      dflt = e;
      continue;
    }
    Type *ty = parse_typename();
    expect(':', "after type in generic association");
    Expr *e = parse_assign();
    if (!chosen && types_compatible(cty, unqual(ty)))
      chosen = e;
  }
  expect(')', "after _Generic");
  if (!chosen)
    chosen = dflt;
  if (!chosen)
    error_tok(t, "controlling expression type '%s' not compatible with any generic association type",
              type_str(cty));
  return chosen;
}

static Expr *compound_literal(Type *ty, Token *tok) {
  // ( type-name ) { initializer-list }
  Type *completed = NULL;
  Expr *e = new_expr(E_COMPLIT, ty, tok);
  if (scope_level() == 0) {
    Sym *s = new_static_local(NULL, ty, tok);
    Init *init = parse_initializer(ty, &completed);
    if (completed)
      s->ty = completed;
    make_static_data(s, init);
    e->sym = s;
    e->ty = s->ty;
    return e;
  }
  Sym *s = new_local(NULL, ty, tok);
  Init *init = parse_initializer(ty, &completed);
  if (completed)
    s->ty = completed;
  e->sym = s;
  e->ty = s->ty;
  e->init = init;
  return e;
}

static Expr *primary(void) {
  Token *t = peek();
  switch (t->kind) {
  case '(': {
    next_tok();
    if (peek()->kind == '{') {
      // GNU statement expression
      if (!current_function())
        error_tok(t, "statement expression not allowed at file scope");
      next_tok();
      enter_scope();
      Stmt *body = parse_compound_stmt_body();
      leave_scope();
      expect(')', "after statement expression");
      Expr *e = new_expr(E_STMTEXPR, ty_void, t);
      e->body = body;
      if (body->nstmts > 0) {
        Stmt *last = body->stmts[body->nstmts - 1];
        if (last->kind == S_EXPR) {
          last->expr = rvalue(last->expr);
          e->ty = last->expr->ty;
        }
      }
      return e;
    }
    Expr *e = parse_expr();
    expect(')', "to match '('");
    return e;
  }
  case TK_INT:
    next_tok();
    return new_iconst(t->ival, t->ty, t);
  case TK_FLOAT:
    next_tok();
    return new_fconst(t->fval, t->ty, t);
  case TK_STR: {
    next_tok();
    Expr *e = new_expr(E_STR, t->ty, t);
    e->sym = new_string_sym(t);
    return e;
  }
  case KW_GENERIC:
    next_tok();
    return generic_selection(t);
  case TK_IDENT: {
    next_tok();
    Sym *s = lookup_ordinary(t->ident);
    if (!s) {
      Expr *b = builtin(t);
      if (b)
        return b;
      char *n = t->ident->name;
      if (str_eq(n, "__func__") || str_eq(n, "__FUNCTION__") || str_eq(n, "__PRETTY_FUNCTION__")) {
        Sym *fs = func_name_sym(t);
        if (!fs)
          error_tok(t, "'%s' is only valid inside a function", n);
        Expr *e = new_expr(E_STR, fs->ty, t);
        e->sym = fs;
        return e;
      }
      if (peek()->kind == '(') {
        // C89 implicit declaration: int name();
        warn_tok(t, "implicit declaration of function '%s' is invalid in C99", n);
        s = xalloc(sizeof(Sym));
        s->kind = SYM_FUNC;
        s->name = t->ident;
        s->ty = func_type(ty_int, NULL, 0, false, false);
        s->tok = t;
        s->is_extern_linkage = true;
        s->asm_name = fmt("_%s", n);
        bind_ordinary(t->ident, s);
        Expr *e = new_expr(E_VAR, s->ty, t);
        e->sym = s;
        return e;
      }
      error_tok(t, "use of undeclared identifier '%s'", n);
    }
    if (s->kind == SYM_ENUMCONST)
      return new_iconst(s->enum_val, s->ty, t);
    if (s->kind == SYM_TYPEDEF)
      error_tok(t, "unexpected type name '%s': expected expression", t->ident->name);
    s->is_used = true;
    Expr *e = new_expr(E_VAR, s->ty, t);
    e->sym = s;
    return e;
  }
  default:
    break;
  }
  if (t->kind == TK_EOF)
    error_tok(t, "expected expression");
  error_tok(t, "expected expression, found %s", t->kind < 128 || t->kind >= KW_FIRST
                                                     ? fmt("'%.*s'", t->len, t->text)
                                                     : kind_name(t->kind));
}

static Expr *postfix(Expr *e) {
  for (;;) {
    Token *t = peek();
    switch (t->kind) {
    case '[': {
      next_tok();
      Expr *idx = parse_expr();
      expect(']', "after array subscript");
      Expr *l = rvalue(e), *r = rvalue(idx);
      if (!(is_ptr(l->ty) && is_integer(r->ty)) && !(is_integer(l->ty) && is_ptr(r->ty)))
        error_tok(t, "subscripted value is not an array or pointer");
      e = deref(add_expr(l, r, t), t);
      continue;
    }
    case '(':
      next_tok();
      e = call(e, t);
      continue;
    case '.': {
      next_tok();
      Token *name = expect(TK_IDENT, "after '.'");
      e = member_access(e, name, t);
      continue;
    }
    case P_ARROW: {
      next_tok();
      Token *name = expect(TK_IDENT, "after '->'");
      Expr *l = rvalue(e);
      if (!is_ptr(l->ty))
        error_tok(t, "member reference type '%s' is not a pointer", type_str(l->ty));
      e = member_access(deref(l, t), name, t);
      continue;
    }
    case P_INC:
    case P_DEC:
      next_tok();
      e = incdec(e, t->kind == P_INC ? 1 : -1, true, t);
      continue;
    default:
      return e;
    }
  }
}

static Expr *sizeof_expr(Token *t) {
  Type *ty;
  if (peek()->kind == '(' && is_type_start(peek_at(1))) {
    next_tok();
    ty = parse_typename();
    expect(')', "after type name");
    if (peek()->kind == '{') {
      Expr *e = postfix(compound_literal(ty, t));
      ty = e->ty;
    }
  } else {
    Expr *e = unary();
    if (is_bitfield(e))
      error_tok(t, "invalid application of 'sizeof' to bit-field");
    ty = e->ty;
  }
  if (ty->kind == TY_FUNC)
    error_tok(t, "invalid application of 'sizeof' to a function type");
  if (ty->kind == TY_VOID)
    return new_iconst(1, ty_ulong, t);
  if (!is_complete(ty))
    error_tok(t, "invalid application of 'sizeof' to an incomplete type '%s'", type_str(ty));
  return new_iconst(ty->size, ty_ulong, t);
}

static Expr *alignof_expr(Token *t) {
  Type *ty;
  if (peek()->kind == '(' && is_type_start(peek_at(1))) {
    next_tok();
    ty = parse_typename();
    expect(')', "after type name");
  } else {
    ty = unary()->ty;
  }
  if (ty->kind == TY_ARRAY)
    ty = ty->base;
  return new_iconst(ty->align, ty_ulong, t);
}

static Expr *unary(void) {
  Token *t = peek();
  switch (t->kind) {
  case P_INC:
  case P_DEC:
    next_tok();
    return incdec(unary(), t->kind == P_INC ? 1 : -1, false, t);
  case '&': {
    next_tok();
    Expr *e = cast_expr();
    // &*p is p and &a[i] is a + i without an access (C11 6.5.3.2p3).
    if (e->kind == E_DEREF) {
      Expr *r = new_expr(E_CAST, pointer_to(e->ty), t);
      r->l = rvalue(e->l);
      return r;
    }
    return address_of(e, t);
  }
  case '*': {
    next_tok();
    Expr *e = cast_expr();
    if (e->ty->kind == TY_FUNC)
      return e; // *f is f
    return deref(e, t);
  }
  case '+': {
    next_tok();
    Expr *e = rvalue(cast_expr());
    if (!is_arith(e->ty))
      error_tok(t, "invalid argument type '%s' to unary expression", type_str(e->ty));
    return promote(e);
  }
  case '-': {
    next_tok();
    Expr *e = rvalue(cast_expr());
    if (!is_arith(e->ty))
      error_tok(t, "invalid argument type '%s' to unary expression", type_str(e->ty));
    e = promote(e);
    if (is_iconst(e))
      return new_iconst((int64_t)(0 - (uint64_t)e->ival), e->ty, t);
    if (is_fconst(e))
      return new_fconst(-e->fval, e->ty, t);
    Expr *n = new_expr(E_NEG, e->ty, t);
    n->l = e;
    return n;
  }
  case '~': {
    next_tok();
    Expr *e = rvalue(cast_expr());
    if (!is_integer(e->ty))
      error_tok(t, "invalid argument type '%s' to unary expression", type_str(e->ty));
    e = promote(e);
    if (is_iconst(e))
      return new_iconst(~e->ival, e->ty, t);
    Expr *n = new_expr(E_BITNOT, e->ty, t);
    n->l = e;
    return n;
  }
  case '!': {
    next_tok();
    Expr *e = cond_expr(cast_expr());
    bool v;
    if (const_truth(e, &v))
      return new_iconst(!v, ty_int, t);
    Expr *n = new_expr(E_NOT, ty_int, t);
    n->l = e;
    return n;
  }
  case KW_SIZEOF:
    next_tok();
    return sizeof_expr(t);
  case KW_ALIGNOF:
    next_tok();
    return alignof_expr(t);
  case KW_EXTENSION:
    next_tok();
    return cast_expr();
  case P_ANDAND:
    error_tok(t, "taking the address of a label is not supported");
  default:
    return postfix(primary());
  }
}

static Expr *cast_expr(void) {
  if (peek()->kind == '(' && is_type_start(peek_at(1))) {
    Token *t = next_tok();
    Type *ty = parse_typename();
    expect(')', "after type name");
    if (peek()->kind == '{')
      return postfix(compound_literal(ty, t));
    Expr *e = cast_expr();
    if (ty->kind != TY_VOID && !is_scalar(unqual(ty)) &&
        !(is_record(ty) && is_record(e->ty) && ty->rec == e->ty->rec))
      error_tok(t, "used type '%s' where arithmetic or pointer type is required", type_str(ty));
    Expr *c = cast_to(e, ty);
    // A cast is never an lvalue, even when no conversion is needed.
    if (is_lvalue(c)) {
      Expr *w = new_expr(E_CAST, unqual(ty), t);
      w->l = c;
      c = w;
    }
    if (c->kind == E_ICONST || c->kind == E_FCONST || c->kind == E_CAST)
      c->tok = t;
    return c;
  }
  return unary();
}

//
// Binary operators by precedence climbing
//

static int binary_prec(int kind) {
  switch (kind) {
  case '*':
  case '/':
  case '%':
    return 10;
  case '+':
  case '-':
    return 9;
  case P_SHL:
  case P_SHR:
    return 8;
  case '<':
  case '>':
  case P_LE:
  case P_GE:
    return 7;
  case P_EQ:
  case P_NE:
    return 6;
  case '&':
    return 5;
  case '^':
    return 4;
  case '|':
    return 3;
  case P_ANDAND:
    return 2;
  case P_OROR:
    return 1;
  default:
    return 0;
  }
}

static Expr *make_binary(Token *op, Expr *l, Expr *r) {
  switch (op->kind) {
  case '*':
    return arith_expr(E_MUL, l, r, op);
  case '/':
    return arith_expr(E_DIV, l, r, op);
  case '%':
    return arith_expr(E_MOD, l, r, op);
  case '+':
    return add_expr(l, r, op);
  case '-':
    return sub_expr(l, r, op);
  case P_SHL:
    return shift_expr(E_SHL, l, r, op);
  case P_SHR:
    return shift_expr(E_SHR, l, r, op);
  case '<':
    return compare_expr(E_LT, l, r, op);
  case '>':
    return compare_expr(E_GT, l, r, op);
  case P_LE:
    return compare_expr(E_LE, l, r, op);
  case P_GE:
    return compare_expr(E_GE, l, r, op);
  case P_EQ:
    return compare_expr(E_EQ, l, r, op);
  case P_NE:
    return compare_expr(E_NE, l, r, op);
  case '&':
    return arith_expr(E_BITAND, l, r, op);
  case '^':
    return arith_expr(E_BITXOR, l, r, op);
  case '|':
    return arith_expr(E_BITOR, l, r, op);
  case P_ANDAND:
    return logical(E_LOGAND, l, r, op);
  case P_OROR:
    return logical(E_LOGOR, l, r, op);
  default:
    ICE("not a binary operator");
  }
}

static Expr *binary(int min_prec) {
  Expr *l = cast_expr();
  for (;;) {
    Token *op = peek();
    int prec = binary_prec(op->kind);
    if (prec == 0 || prec < min_prec)
      return l;
    next_tok();
    Expr *r = binary(prec + 1);
    l = make_binary(op, l, r);
  }
}

Expr *parse_cond_expr(void) {
  Expr *c = binary(1);
  Token *q = peek();
  if (!accept('?'))
    return c;
  if (peek()->kind == ':')
    error_tok(peek(), "the GNU 'a ?: b' extension is not supported");
  Expr *a = parse_expr();
  expect(':', "in conditional expression");
  Expr *b = parse_cond_expr();
  return conditional(c, a, b, q);
}

static ExprKind assign_op(int kind) {
  switch (kind) {
  case P_ADD_EQ:
    return E_ADD;
  case P_SUB_EQ:
    return E_SUB;
  case P_MUL_EQ:
    return E_MUL;
  case P_DIV_EQ:
    return E_DIV;
  case P_MOD_EQ:
    return E_MOD;
  case P_SHL_EQ:
    return E_SHL;
  case P_SHR_EQ:
    return E_SHR;
  case P_AND_EQ:
    return E_BITAND;
  case P_OR_EQ:
    return E_BITOR;
  case P_XOR_EQ:
    return E_BITXOR;
  default:
    return E_ICONST; // not an assignment operator
  }
}

Expr *parse_assign(void) {
  Expr *l = parse_cond_expr();
  Token *t = peek();
  if (t->kind == '=') {
    next_tok();
    return assign_expr(l, parse_assign(), t);
  }
  ExprKind op = assign_op(t->kind);
  if (op != E_ICONST) {
    next_tok();
    return op_assign(op, l, parse_assign(), t);
  }
  return l;
}

Expr *parse_expr(void) {
  Expr *e = parse_assign();
  while (peek()->kind == ',') {
    Token *t = next_tok();
    Expr *r = parse_assign();
    Expr *c = new_expr(E_COMMA, rvalue(r)->ty, t);
    c->l = e;
    c->r = rvalue(r);
    e = c;
  }
  return e;
}
