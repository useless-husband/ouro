// Initializers (C11 6.7.9).
//
// An initializer is parsed into a tree that mirrors the object: one node
// per array element or struct member that is explicitly initialised.
// The hard parts are brace elision (an aggregate member initialised
// without its own braces takes as many values as it needs from the
// enclosing list) and designators, after which initialisation continues
// with the next subobject of the innermost aggregate designated.
//
// Static objects are then turned into a byte image plus relocations;
// automatic objects are initialised by code (irgen.c).

#include "ouro.h"

static void init_one(Init *sub);
static Expr *pending; // an initializer already parsed whose target is not known yet

static int64_t capacity(Init *init) {
  if (init->ty->kind == TY_ARRAY)
    return init->ty->len; // -1: unknown, grows
  return init->ty->rec->nmembers;
}

static Init *new_init(Type *ty, Token *tok) {
  Init *init = xalloc(sizeof(Init));
  init->ty = ty;
  init->tok = tok;
  if (ty->kind == TY_ARRAY && ty->len >= 0) {
    init->nelems = ty->len;
    init->elems = xalloc(sizeof(Init *) * (ty->len + 1));
  } else if (is_record(ty)) {
    init->nelems = ty->rec->nmembers;
    init->elems = xalloc(sizeof(Init *) * (init->nelems + 1));
  }
  return init;
}

static Member *member_at(Type *ty, int64_t idx) {
  for (Member *m = ty->rec->members; m; m = m->next)
    if (m->idx == idx)
      return m;
  return NULL;
}

// The element `idx` of an aggregate initializer, created on first use.
// Arrays of unknown length grow.
static Init *child(Init *init, int64_t idx) {
  if (init->ty->kind == TY_ARRAY && init->ty->len < 0 && idx >= init->nelems) {
    int64_t n = init->nelems ? init->nelems : 8;
    while (n <= idx)
      n *= 2;
    init->elems = xgrow(init->elems, sizeof(Init *) * init->nelems, sizeof(Init *) * (n + 1));
    // nelems tracks the number of slots; the used length is computed later
    init->nelems = n;
  }
  if (!init->elems[idx]) {
    Type *ty = init->ty->kind == TY_ARRAY ? init->ty->base : member_at(init->ty, idx)->ty;
    init->elems[idx] = new_init(ty, peek());
  }
  if (init->ty->kind == TY_UNION)
    init->union_mem = member_at(init->ty, idx);
  return init->elems[idx];
}

// Positional initialisation skips unnamed bit-fields (C11 6.7.9p9) and,
// in a union, everything after the first member.
static int64_t next_index(Init *init, int64_t idx) {
  if (init->ty->kind == TY_ARRAY)
    return idx;
  if (init->ty->kind == TY_UNION)
    return idx == 0 ? 0 : init->ty->rec->nmembers;
  for (;;) {
    Member *m = member_at(init->ty, idx);
    if (!m || m->name || !m->is_bitfield)
      return idx;
    idx++;
  }
}

static bool full(Init *init, int64_t idx) {
  int64_t cap = capacity(init);
  return cap >= 0 && idx >= cap;
}

static bool is_char_array(Type *ty) {
  return ty->kind == TY_ARRAY && is_integer(ty->base) &&
         (is_char_type(ty->base) || ty->base->size == 2 || ty->base->size == 4);
}

static bool string_fits_array(Type *arr, Token *str) {
  Type *elem = str->ty->base;
  return arr->base->size == elem->size;
}

// Reads past an excess initializer.
static void skip_initializer(void) {
  if (pending) {
    pending = NULL;
    return;
  }
  if (accept('{')) {
    int d = 1;
    while (d > 0) {
      Token *t = next_tok();
      if (t->kind == '{')
        d++;
      else if (t->kind == '}')
        d--;
      else if (t->kind == TK_EOF)
        error_tok(t, "unterminated initializer list");
    }
    return;
  }
  parse_assign();
}

static bool at_designator(void) {
  return peek()->kind == '.' || peek()->kind == '[';
}

// After one element, decides whether the elided list continues: a comma
// followed by something that is neither '}' nor a designator.
static bool elision_continues(void) {
  if (peek()->kind != ',')
    return false;
  Token *t = peek_at(1);
  return t->kind != '}' && t->kind != '.' && t->kind != '[';
}

// Fills `init` (an aggregate) from position `idx` with brace elision:
// values come from the enclosing list until the aggregate is full or the
// list moves on.
static void elided_fill(Init *init, int64_t idx, bool need_comma) {
  for (;;) {
    idx = next_index(init, idx);
    if (full(init, idx))
      return;
    if (need_comma) {
      if (!elision_continues())
        return;
      next_tok();
    }
    init_one(child(init, idx));
    need_comma = true;
    idx++;
    if (init->ty->kind == TY_UNION)
      return;
  }
}

// Parses designators that start at aggregate `init`, then the value of
// the designated subobject.  Returns the index designated in `init`.
static int64_t designation(Init *init, bool top) {
  Token *t = peek();
  int64_t idx;
  Init *sub;
  if (accept('[')) {
    if (init->ty->kind != TY_ARRAY)
      error_tok(t, "array designator cannot initialize non-array type '%s'", type_str(init->ty));
    Token *it = peek();
    idx = parse_const_int();
    int64_t hi = idx;
    if (accept(P_ELLIPSIS))
      hi = parse_const_int();
    expect(']', "after array designator");
    if (idx < 0 || (init->ty->len >= 0 && hi >= init->ty->len))
      error_tok(it, "array designator index (%lld) exceeds array bounds (%lld)", (long long)hi,
                (long long)init->ty->len);
    if (hi != idx) {
      // GNU range designator [lo ... hi] = value: the same value for each.
      if (at_designator())
        error_tok(t, "nested designators after a range designator are not supported");
      expect('=', "after designator");
      int save = P.pos;
      for (int64_t i = idx; i <= hi; i++) {
        P.pos = save; // the same value for each element
        init_one(child(init, i));
      }
      idx = hi;
      if (!top)
        elided_fill(init, idx + 1, true);
      return idx;
    }
    sub = child(init, idx);
  } else {
    next_tok(); // '.'
    Token *nt = expect(TK_IDENT, "after '.' in designator");
    if (!is_record(init->ty))
      error_tok(t, "field designator cannot initialize a non-struct, non-union type '%s'",
                type_str(init->ty));
    Vec path = {0};
    Member *m = find_member(init->ty, nt->ident, &path);
    if (!m)
      error_tok(nt, "field designator '%s' does not refer to any field in type '%s'",
                nt->ident->name, type_str(init->ty));
    // Walk through anonymous members: each is an implicit designator.
    Init *cur = init;
    int64_t top_idx = -1;
    for (int i = 0; i <= path.len; i++) {
      Member *step = i < path.len ? path.data[i] : m;
      if (top_idx < 0)
        top_idx = step->idx;
      Init *c = child(cur, step->idx);
      if (i < path.len)
        cur = c;
      else
        sub = c;
      idx = step->idx;
    }
    if (path.len > 0) {
      // The designated member is inside anonymous members.
      if (at_designator())
        designation(sub, false);
      else {
        expect('=', "after designator");
        init_one(sub);
      }
      elided_fill(cur, idx + 1, true);
      if (!top)
        elided_fill(init, top_idx + 1, true);
      return top_idx;
    }
  }
  if (at_designator()) {
    if (!is_aggregate(sub->ty))
      error_tok(peek(), "designator into non-aggregate type '%s'", type_str(sub->ty));
    designation(sub, false);
  } else {
    expect('=', "after designator");
    init_one(sub);
  }
  if (!top)
    elided_fill(init, idx + 1, true);
  return idx;
}

// { initializer-list } for an aggregate; the '{' has been read.
static void braced_list(Init *init) {
  int64_t idx = 0;
  bool first = true;
  for (;;) {
    if (accept('}'))
      return;
    if (!first) {
      expect(',', "between initializers");
      if (accept('}'))
        return;
    }
    first = false;
    if (at_designator()) {
      idx = designation(init, true) + 1;
      continue;
    }
    idx = next_index(init, idx);
    if (full(init, idx)) {
      warn_tok(peek(), "excess elements in %s initializer",
               init->ty->kind == TY_ARRAY ? "array" : init->ty->kind == TY_UNION ? "union" : "struct");
      skip_initializer();
      continue;
    }
    init_one(child(init, idx));
    idx++;
  }
}

static void string_init(Init *init, Expr *str) {
  Token *t = str->tok;
  if (!string_fits_array(init->ty, t))
    error_tok(t, "initializing '%s' with a string literal of type '%s'", type_str(init->ty),
              type_str(t->ty));
  init->expr = str;
}

static void init_one(Init *sub) {
  Type *ty = sub->ty;
  Token *t = peek();
  sub->tok = t;
  if (!pending && t->kind == '{') {
    next_tok();
    if (is_aggregate(ty)) {
      if (is_char_array(ty) && peek()->kind == TK_STR && (peek_at(1)->kind == '}' ||
                                                          (peek_at(1)->kind == ',' && peek_at(2)->kind == '}'))) {
        // char s[] = {"abc"}
        Token *st = next_tok();
        Expr *e = new_expr(E_STR, st->ty, st);
        e->sym = new_string_sym(st);
        string_init(sub, e);
        accept(',');
        expect('}', "after string initializer");
        return;
      }
      braced_list(sub);
      return;
    }
    // A scalar in braces: { value }.
    if (accept('}')) {
      warn_tok(t, "scalar initializer is empty");
      return;
    }
    init_one(sub);
    while (accept(',')) {
      if (peek()->kind == '}')
        break;
      warn_tok(peek(), "excess elements in scalar initializer");
      skip_initializer();
    }
    expect('}', "after scalar initializer");
    return;
  }

  if (is_aggregate(ty)) {
    if (!pending) {
      Expr *e = parse_assign();
      pending = e;
    }
    Expr *e = pending;
    if (is_record(ty) && is_record(e->ty) && e->ty->rec == ty->rec) {
      pending = NULL;
      sub->expr = e;
      return;
    }
    if (is_char_array(ty) && e->kind == E_STR && string_fits_array(ty, e->tok)) {
      pending = NULL;
      string_init(sub, e);
      return;
    }
    if (is_record(ty) && !ty->rec->complete)
      error_tok(t, "variable has incomplete type '%s'", type_str(ty));
    // Brace elision: the value initialises the first scalar inside.
    elided_fill(sub, 0, false);
    return;
  }

  Expr *e = pending ? pending : parse_assign();
  pending = NULL;
  sub->expr = assign_conv(e, ty, e->tok, "initializing");
}

// Parses the initializer of an object of type `ty`.  For an array of
// unknown length the completed type is stored in *completed.
Init *parse_initializer(Type *ty, Type **completed) {
  *completed = NULL;
  pending = NULL;
  Token *t = peek();
  if (ty->kind == TY_FUNC)
    error_tok(t, "illegal initializer (only variables can be initialized)");
  if (is_record(ty) && !ty->rec->complete)
    error_tok(t, "variable has incomplete type '%s'", type_str(ty));
  if (ty->kind == TY_ARRAY && !is_complete(ty->base))
    error_tok(t, "array has incomplete element type '%s'", type_str(ty->base));

  Init *init = new_init(ty, t);
  if (ty->kind == TY_ARRAY && t->kind == TK_STR && is_char_array(ty)) {
    Token *st = next_tok();
    Expr *e = new_expr(E_STR, st->ty, st);
    e->sym = new_string_sym(st);
    string_init(init, e);
  } else if (is_aggregate(ty) && t->kind != '{') {
    Expr *e = parse_assign();
    if (is_record(ty) && is_record(e->ty) && e->ty->rec == ty->rec) {
      init->expr = e;
    } else {
      error_tok(t, "initializing '%s' requires a braced initializer list", type_str(ty));
    }
  } else {
    init_one(init);
  }

  if (ty->kind == TY_ARRAY && ty->len < 0) {
    int64_t len = 0;
    if (init->expr && init->expr->kind == E_STR) {
      len = init->expr->ty->len;
    } else {
      for (int64_t i = 0; i < init->nelems; i++)
        if (init->elems[i])
          len = i + 1;
    }
    Type *nt = array_of(ty->base, len);
    init->ty = nt;
    init->nelems = len;
    *completed = nt;
  }
  return init;
}

//
// Static data
//

static void write_int(char *buf, int64_t v, int size) {
  for (int i = 0; i < size; i++)
    buf[i] = (char)(v >> (i * 8));
}

static int64_t read_int(char *buf, int size) {
  uint64_t v = 0;
  for (int i = 0; i < size; i++)
    v |= (uint64_t)(unsigned char)buf[i] << (i * 8);
  return (int64_t)v;
}

static char *data_buf;
static Reloc **reloc_tail;

// Writes the value of `init` at data_buf + off.  `bf` is set when the
// value goes into a bit-field.
static void write_data(int64_t off, Init *init, Member *bf) {
  if (!init)
    return;
  char *buf = data_buf + off;
  Type *ty = init->ty;
  if (init->expr && init->expr->kind == E_STR && ty->kind == TY_ARRAY) {
    Token *st = init->expr->tok;
    int64_t n = st->str_bytes;
    if (ty->size < n)
      n = ty->size; // C allows the terminator to be dropped when it does not fit
    if (ty->size + st->ty->base->size < st->str_bytes)
      warn_tok(st, "initializer-string for char array is too long");
    memcpy(buf, st->str, n);
    return;
  }
  if (ty->kind == TY_ARRAY) {
    for (int64_t i = 0; i < init->nelems; i++)
      write_data(off + i * ty->base->size, init->elems[i], NULL);
    return;
  }
  if (is_record(ty)) {
    if (init->expr) {
      // A compound literal at file scope has a static image: copy it
      // (GNU C accepts this as a constant initializer).
      Expr *e = init->expr;
      if (e->kind == E_COMPLIT && e->sym->is_global && e->ty->size == ty->size) {
        if (e->sym->data)
          memcpy(buf, e->sym->data, ty->size);
        for (Reloc *r = e->sym->relocs; r; r = r->next) {
          Reloc *c = xalloc(sizeof(Reloc));
          *c = *r;
          c->next = NULL;
          c->offset = off + r->offset;
          *reloc_tail = c;
          reloc_tail = &c->next;
        }
        return;
      }
      error_tok(e->tok, "initializer element is not a compile-time constant");
    }
    for (Member *m = ty->rec->members; m; m = m->next) {
      if (ty->kind == TY_UNION && init->union_mem != m)
        continue;
      if (init->elems[m->idx])
        write_data(off + m->offset, init->elems[m->idx], m->is_bitfield ? m : NULL);
    }
    return;
  }
  Expr *e = init->expr;
  if (!e)
    return;
  if (is_flonum(ty)) {
    double v;
    if (!eval_float_const(e, &v))
      error_tok(e->tok, "initializer element is not a compile-time constant");
    if (ty->kind == TY_FLOAT) {
      float f = (float)v;
      memcpy(buf, &f, 4);
    } else {
      memcpy(buf, &v, 8);
    }
    return;
  }
  int64_t v;
  if (eval_int_const(e, &v)) {
    if (bf) {
      int unit = (int)bf->ty->size;
      uint64_t word = (uint64_t)read_int(buf, unit);
      uint64_t mask = bf->bit_width == 64 ? ~0ULL : ((1ULL << bf->bit_width) - 1);
      word = (word & ~(mask << bf->bit_offset)) | (((uint64_t)v & mask) << bf->bit_offset);
      write_int(buf, (int64_t)word, unit);
      return;
    }
    write_int(buf, v, (int)ty->size);
    return;
  }
  Sym *sym;
  int64_t addend;
  if (ty->size == 8 && eval_static_addr(e, &sym, &addend)) {
    if (!sym) {
      write_int(buf, addend, 8);
      return;
    }
    sym->is_used = true;
    Reloc *r = xalloc(sizeof(Reloc));
    r->offset = off;
    r->sym = sym;
    r->addend = addend;
    *reloc_tail = r;
    reloc_tail = &r->next;
    return;
  }
  error_tok(e->tok, "initializer element is not a compile-time constant");
}

void make_static_data(Sym *var, Init *init) {
  int64_t size = var->ty->size;
  if (size < 0)
    error_tok(var->tok, "variable has incomplete type '%s'", type_str(var->ty));
  init->ty = var->ty;
  data_buf = xalloc(size > 0 ? size : 1);
  Reloc head;
  memset(&head, 0, sizeof(head));
  reloc_tail = &head.next;
  write_data(0, init, NULL);
  var->relocs = head.next;
  // An all-zero image without relocations goes to the zero-fill section.
  bool zero = true;
  for (int64_t i = 0; i < size && zero; i++)
    if (data_buf[i])
      zero = false;
  var->data = (zero && !var->relocs) ? NULL : data_buf;
  var->is_defined = true;
  var->is_tentative = false;
}
