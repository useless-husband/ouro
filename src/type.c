// C types: construction, layout, compatibility and the usual
// conversions.  The target is LP64 with a signed plain char and an
// 8-byte long double.

#include "ouro.h"

Type *ty_void, *ty_bool, *ty_char, *ty_schar, *ty_uchar, *ty_short, *ty_ushort, *ty_int, *ty_uint,
    *ty_long, *ty_ulong, *ty_llong, *ty_ullong, *ty_float, *ty_double, *ty_ldouble;

static Type *basic(TypeKind kind, int size, bool is_unsigned) {
  Type *t = xalloc(sizeof(Type));
  t->kind = kind;
  t->size = size;
  t->align = size ? size : 1;
  t->is_unsigned = is_unsigned;
  t->unqual = t;
  return t;
}

void type_init(void) {
  if (ty_int)
    return;
  ty_void = basic(TY_VOID, 1, false);
  ty_void->size = 1; // sizeof(void) is 1 as a GNU extension (pointer arithmetic)
  ty_bool = basic(TY_BOOL, 1, true);
  ty_char = basic(TY_CHAR, 1, false);
  ty_schar = basic(TY_SCHAR, 1, false);
  ty_uchar = basic(TY_UCHAR, 1, true);
  ty_short = basic(TY_SHORT, 2, false);
  ty_ushort = basic(TY_USHORT, 2, true);
  ty_int = basic(TY_INT, 4, false);
  ty_uint = basic(TY_UINT, 4, true);
  ty_long = basic(TY_LONG, 8, false);
  ty_ulong = basic(TY_ULONG, 8, true);
  ty_llong = basic(TY_LLONG, 8, false);
  ty_ullong = basic(TY_ULLONG, 8, true);
  ty_float = basic(TY_FLOAT, 4, false);
  ty_double = basic(TY_DOUBLE, 8, false);
  ty_ldouble = basic(TY_LDOUBLE, 8, false);
}

bool is_integer(Type *t) {
  return (t->kind >= TY_BOOL && t->kind <= TY_ULLONG) || t->kind == TY_ENUM;
}

bool is_flonum(Type *t) {
  return t->kind == TY_FLOAT || t->kind == TY_DOUBLE || t->kind == TY_LDOUBLE;
}

bool is_arith(Type *t) {
  return is_integer(t) || is_flonum(t);
}

bool is_ptr(Type *t) {
  return t->kind == TY_PTR;
}

bool is_scalar(Type *t) {
  return is_arith(t) || is_ptr(t);
}

bool is_record(Type *t) {
  return t->kind == TY_STRUCT || t->kind == TY_UNION;
}

bool is_aggregate(Type *t) {
  return is_record(t) || t->kind == TY_ARRAY;
}

bool is_func(Type *t) {
  return t->kind == TY_FUNC;
}

bool is_array(Type *t) {
  return t->kind == TY_ARRAY;
}

bool is_void(Type *t) {
  return t->kind == TY_VOID;
}

bool is_unsigned_int(Type *t) {
  return is_integer(t) && t->is_unsigned;
}

bool is_complete(Type *t) {
  if (t->kind == TY_VOID)
    return false;
  if (is_record(t) || t->kind == TY_ENUM)
    return t->rec->complete;
  if (t->kind == TY_ARRAY)
    return (t->len >= 0 || t->is_vla) && is_complete(t->base);
  return true;
}

bool is_char_type(Type *t) {
  return t->kind == TY_CHAR || t->kind == TY_SCHAR || t->kind == TY_UCHAR;
}

// Integer conversion rank (C11 6.3.1.1); long and long long are
// distinct types with distinct ranks even though both are 64-bit.
int int_rank(Type *t) {
  switch (t->kind) {
  case TY_BOOL:
    return 1;
  case TY_CHAR:
  case TY_SCHAR:
  case TY_UCHAR:
    return 2;
  case TY_SHORT:
  case TY_USHORT:
    return 3;
  case TY_INT:
  case TY_UINT:
  case TY_ENUM:
    return 4;
  case TY_LONG:
  case TY_ULONG:
    return 5;
  case TY_LLONG:
  case TY_ULLONG:
    return 6;
  default:
    return 0;
  }
}

static Type *new_type(TypeKind kind, int64_t size, int align) {
  Type *t = xalloc(sizeof(Type));
  t->kind = kind;
  t->size = size;
  t->align = align;
  t->unqual = t;
  return t;
}

Type *pointer_to(Type *base) {
  if (base->ptr_to)
    return base->ptr_to;
  Type *t = new_type(TY_PTR, 8, 8);
  t->base = base;
  t->is_unsigned = true;
  base->ptr_to = t;
  return t;
}

Type *array_of(Type *base, int64_t len) {
  Type *t = new_type(TY_ARRAY, len >= 0 ? base->size * len : -1, base->align);
  t->base = base;
  t->len = len;
  return t;
}

Type *func_type(Type *ret, Type **params, int nparams, bool variadic, bool has_proto) {
  Type *t = new_type(TY_FUNC, 1, 1);
  t->ret = ret;
  t->params = params;
  t->nparams = nparams;
  t->variadic = variadic;
  t->has_proto = has_proto;
  return t;
}

// Returns `t` with the qualifiers `qual` added.  Qualifiers on an array
// type apply to its elements (C11 6.7.3p9).
Type *qualified(Type *t, int qual) {
  if (!qual || (t->qual & qual) == qual)
    return t;
  if (t->kind == TY_ARRAY) {
    Type *a = array_of(qualified(t->base, qual), t->len);
    return a;
  }
  Type *q = xalloc(sizeof(Type));
  *q = *t;
  q->qual = t->qual | qual;
  q->unqual = t->unqual;
  q->ptr_to = NULL;
  return q;
}

Type *unqual(Type *t) {
  return t->unqual ? t->unqual : t;
}

Type *new_record_type(TypeKind kind, Ident *tag, Token *tok) {
  Type *t = new_type(kind, -1, 1);
  t->rec = xalloc(sizeof(Record));
  t->rec->tag = tag;
  t->rec->tok = tok;
  return t;
}

Type *enum_type(Ident *tag, Token *tok) {
  Type *t = new_type(TY_ENUM, 4, 4);
  t->rec = xalloc(sizeof(Record));
  t->rec->tag = tag;
  t->rec->tok = tok;
  t->rec->is_enum = true;
  t->rec->enum_base = ty_uint; // until the enumerators say otherwise
  t->is_unsigned = true;
  return t;
}

// Type compatibility (C11 6.2.7), ignoring top-level qualifiers only
// where the caller asks for that by passing unqualified types.
bool types_compatible(Type *a, Type *b) {
  if (a == b)
    return true;
  if (a->qual != b->qual)
    return false;
  if (a->kind == TY_ENUM && is_integer(b) && b->kind != TY_ENUM)
    return types_compatible(a->rec->enum_base, b);
  if (b->kind == TY_ENUM && is_integer(a) && a->kind != TY_ENUM)
    return types_compatible(a, b->rec->enum_base);
  if (a->kind != b->kind)
    return false;
  switch (a->kind) {
  case TY_PTR:
    return types_compatible(a->base, b->base);
  case TY_ARRAY:
    if (!types_compatible(a->base, b->base))
      return false;
    return a->len < 0 || b->len < 0 || a->len == b->len;
  case TY_FUNC:
    if (!types_compatible(unqual(a->ret), unqual(b->ret)))
      return false;
    if (!a->has_proto || !b->has_proto)
      return true;
    if (a->nparams != b->nparams || a->variadic != b->variadic)
      return false;
    for (int i = 0; i < a->nparams; i++)
      if (!types_compatible(unqual(a->params[i]), unqual(b->params[i])))
        return false;
    return true;
  case TY_STRUCT:
  case TY_UNION:
  case TY_ENUM:
    return a->rec == b->rec;
  default:
    return true; // same basic type
  }
}

// The composite type of two compatible types (C11 6.2.7p3): the more
// informative of the two.
Type *composite_type(Type *a, Type *b) {
  if (a->kind == TY_ARRAY && b->kind == TY_ARRAY) {
    if (a->len < 0 && b->len >= 0)
      return b;
    return a;
  }
  if (a->kind == TY_FUNC && b->kind == TY_FUNC) {
    if (!a->has_proto && b->has_proto)
      return b;
    return a;
  }
  return a;
}

// Integer promotion (C11 6.3.1.1p2).
Type *promote_int(Type *t) {
  if (t->kind == TY_ENUM)
    return unqual(t->rec->enum_base);
  if (is_integer(t) && int_rank(t) < int_rank(ty_int))
    return ty_int; // every narrower type fits in int
  return unqual(t);
}

// The usual arithmetic conversions (C11 6.3.1.8).
Type *arith_conv(Type *a, Type *b) {
  if (a->kind == TY_LDOUBLE || b->kind == TY_LDOUBLE)
    return ty_ldouble;
  if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE)
    return ty_double;
  if (a->kind == TY_FLOAT || b->kind == TY_FLOAT)
    return ty_float;
  a = promote_int(a);
  b = promote_int(b);
  if (a->kind == b->kind)
    return a;
  if (a->is_unsigned == b->is_unsigned)
    return int_rank(a) >= int_rank(b) ? a : b;
  Type *u = a->is_unsigned ? a : b;
  Type *s = a->is_unsigned ? b : a;
  if (int_rank(u) >= int_rank(s))
    return u;
  if (s->size > u->size)
    return s;
  // The signed type cannot represent every value of the unsigned one:
  // use the unsigned counterpart of the signed type.
  switch (s->kind) {
  case TY_LONG:
    return ty_ulong;
  case TY_LLONG:
    return ty_ullong;
  default:
    return ty_uint;
  }
}

// Array-to-pointer and function-to-pointer conversion.
Type *decay(Type *t) {
  if (t->kind == TY_ARRAY)
    return pointer_to(t->base);
  if (t->kind == TY_FUNC)
    return pointer_to(t);
  return t;
}

int64_t type_max_uint(Type *t) {
  if (t->size >= 8)
    return -1;
  return ((int64_t)1 << (t->size * 8)) - 1;
}

//
// Printing types for diagnostics
//

static void type_str2(Buf *b, Type *t, const char *inner);

static void quals_str(Buf *b, int q) {
  if (q & Q_CONST)
    buf_puts(b, "const ");
  if (q & Q_VOLATILE)
    buf_puts(b, "volatile ");
  if (q & Q_RESTRICT)
    buf_puts(b, "restrict ");
}

static const char *basic_name(Type *t) {
  switch (t->kind) {
  case TY_VOID:
    return "void";
  case TY_BOOL:
    return "_Bool";
  case TY_CHAR:
    return "char";
  case TY_SCHAR:
    return "signed char";
  case TY_UCHAR:
    return "unsigned char";
  case TY_SHORT:
    return "short";
  case TY_USHORT:
    return "unsigned short";
  case TY_INT:
    return "int";
  case TY_UINT:
    return "unsigned int";
  case TY_LONG:
    return "long";
  case TY_ULONG:
    return "unsigned long";
  case TY_LLONG:
    return "long long";
  case TY_ULLONG:
    return "unsigned long long";
  case TY_FLOAT:
    return "float";
  case TY_DOUBLE:
    return "double";
  case TY_LDOUBLE:
    return "long double";
  default:
    return NULL;
  }
}

static void type_str2(Buf *b, Type *t, const char *inner) {
  switch (t->kind) {
  case TY_PTR: {
    Buf in = {0};
    bool paren = t->base->kind == TY_ARRAY || t->base->kind == TY_FUNC;
    if (paren)
      buf_putc(&in, '(');
    buf_putc(&in, '*');
    if (t->qual)
      buf_putc(&in, ' ');
    if (t->qual & Q_CONST)
      buf_puts(&in, "const");
    if (t->qual & Q_VOLATILE)
      buf_puts(&in, (t->qual & Q_CONST) ? " volatile" : "volatile");
    if (inner[0] && t->qual)
      buf_putc(&in, ' ');
    buf_puts(&in, inner);
    if (paren)
      buf_putc(&in, ')');
    type_str2(b, t->base, buf_cstr(&in));
    return;
  }
  case TY_ARRAY: {
    char *s = t->len >= 0 ? fmt("%s[%lld]", inner, (long long)t->len) : fmt("%s[]", inner);
    type_str2(b, t->base, s);
    return;
  }
  case TY_FUNC: {
    Buf in = {0};
    buf_puts(&in, inner);
    buf_putc(&in, '(');
    for (int i = 0; i < t->nparams; i++) {
      if (i)
        buf_puts(&in, ", ");
      buf_puts(&in, type_str(t->params[i]));
    }
    if (t->variadic)
      buf_puts(&in, t->nparams ? ", ..." : "...");
    else if (t->has_proto && t->nparams == 0)
      buf_puts(&in, "void");
    buf_putc(&in, ')');
    type_str2(b, t->ret, buf_cstr(&in));
    return;
  }
  default:
    break;
  }
  quals_str(b, t->qual);
  const char *name = basic_name(t);
  if (name) {
    buf_puts(b, name);
  } else {
    const char *kw = t->kind == TY_STRUCT ? "struct" : t->kind == TY_UNION ? "union" : "enum";
    if (t->rec->tag)
      buf_printf(b, "%s %s", kw, t->rec->tag->name);
    else
      buf_printf(b, "%s (anonymous)", kw);
  }
  if (inner[0]) {
    buf_putc(b, ' ');
    buf_puts(b, inner);
  }
}

char *type_str(Type *t) {
  Buf b = {0};
  type_str2(&b, t, "");
  return buf_cstr(&b);
}

//
// Struct and union layout
//

// Lays out the members of a struct or union following the AAPCS64 rules
// that Apple's ABI uses (and clang implements): members at their natural
// alignment; a bit-field is placed in the next bits if it fits within a
// naturally aligned storage unit of its declared type, otherwise it
// starts a new unit; a zero-width bit-field rounds up to its type's
// alignment; unnamed bit-fields do not affect the record's alignment.
void layout_record(Type *t, Token *tok) {
  Record *r = t->rec;
  bool is_union = t->kind == TY_UNION;
  int64_t bits = 0; // current position in bits (struct)
  int64_t size_bits = 0;
  int align = 1;
  int idx = 0;

  for (Member *m = r->members; m; m = m->next) {
    m->idx = idx++;
    Type *mt = m->ty;
    int malign = r->packed ? 1 : mt->align;

    if (mt->size < 0 && !(mt->kind == TY_ARRAY && !m->next && !is_union))
      error_tok(m->tok ? m->tok : tok, "field has incomplete type '%s'", type_str(mt));

    if (m->is_bitfield) {
      int64_t unit = mt->size * 8;
      if (m->bit_width == 0) {
        // Zero-width: move to the next unit boundary.
        if (!is_union)
          bits = align_up(bits, unit);
        m->offset = bits / 8;
        m->bit_offset = 0;
        continue;
      }
      if (is_union) {
        m->offset = 0;
        m->bit_offset = 0;
        if (m->bit_width > size_bits)
          size_bits = m->bit_width;
      } else {
        if (!r->packed && (bits / unit) != ((bits + m->bit_width - 1) / unit))
          bits = align_up(bits, unit);
        // The storage unit is the aligned unit containing the field.
        int64_t unit_start = r->packed ? (bits / 8) * 8 : (bits / unit) * unit;
        m->offset = unit_start / 8;
        m->bit_offset = (int)(bits - unit_start);
        bits += m->bit_width;
        if (bits > size_bits)
          size_bits = bits;
      }
      if (m->name && malign > align)
        align = malign;
      continue;
    }

    if (is_union) {
      m->offset = 0;
      int64_t sz = mt->size < 0 ? 0 : mt->size * 8;
      if (sz > size_bits)
        size_bits = sz;
    } else {
      bits = align_up(bits, (int64_t)malign * 8);
      m->offset = bits / 8;
      if (mt->size > 0)
        bits += mt->size * 8;
      if (bits > size_bits)
        size_bits = bits;
    }
    if (malign > align)
      align = malign;
  }
  r->nmembers = idx;
  if (t->align > align)
    align = t->align; // _Alignas / __attribute__((aligned))
  t->align = align;
  t->size = align_up(align_up(size_bits, 8) / 8, align);
  r->complete = true;
}

// Finds a member by name, looking inside anonymous struct and union
// members.  The anonymous members passed through are appended to `path`
// (outermost first), so the caller can build the access step by step.
Member *find_member(Type *rec, Ident *name, Vec *path) {
  for (Member *m = rec->rec->members; m; m = m->next) {
    if (m->name == name)
      return m;
    if (!m->name && is_record(m->ty) && !m->is_bitfield) {
      int saved = path->len;
      vec_push(path, m);
      Member *found = find_member(m->ty, name, path);
      if (found)
        return found;
      path->len = saved;
    }
  }
  return NULL;
}
