// The parser: declarations, statements and the translation unit.
// Expressions are in expr.c and initializers in init.c.
//
// Parsing and semantic analysis happen in one pass.  Scopes are kept on
// the identifiers themselves: every Ident points to its innermost
// binding, and leaving a scope restores the bindings it shadowed.

#include "ouro.h"

Parser P;

static Sym *program_head;
static Sym *program_tail;
static Vec func_defs;
static Sym *cur_fn;
static int depth;            // 0 = file scope
static Vec scope_stack;      // Binding* lists, one per open scope
static Vec labels;           // Label* of the current function
static Stmt *cur_switch;
static Vec *cur_cases;       // S_CASE statements of the innermost switch
static int loop_depth;       // enclosing loops (for continue)
static int break_depth;      // enclosing loops and switches (for break)
static int anon_counter;

static Type *declspec(void *attr);
static Stmt *statement(void);

//
// Tokens
//

Token *peek(void) {
  return P.toks[P.pos];
}

Token *peek_at(int k) {
  Token *t = P.toks[P.pos];
  int i = P.pos;
  while (k-- > 0 && t->kind != TK_EOF)
    t = P.toks[++i];
  return t;
}

Token *next_tok(void) {
  Token *t = P.toks[P.pos];
  if (t->kind != TK_EOF)
    P.pos++;
  return t;
}

bool accept(int kind) {
  if (peek()->kind == kind) {
    P.pos++;
    return true;
  }
  return false;
}

static const char *tok_desc(Token *t) {
  if (t->kind == TK_EOF)
    return "end of file";
  return fmt("'%.*s'", t->len, t->text);
}

Token *expect(int kind, const char *what) {
  Token *t = peek();
  if (t->kind != kind) {
    if (kind == ';' || kind == ')' || kind == ']' || kind == '}') {
      // Point just after the previous token, where the missing
      // punctuator belongs (clang's convention).
      Token *prev = P.pos > 0 ? P.toks[P.pos - 1] : t;
      if (prev->line != t->line || prev->file != t->file) {
        Token *at = copy_token(prev);
        at->loc = prev->loc + prev->len;
        error_tok(at, "expected '%s' %s", kind_name(kind), what);
      }
    }
    error_tok(t, "expected '%s' %s, found %s", kind_name(kind), what, tok_desc(t));
  }
  P.pos++;
  return t;
}

static Ident *expect_ident(const char *what) {
  Token *t = peek();
  if (t->kind != TK_IDENT)
    error_tok(t, "expected %s, found %s", what, tok_desc(t));
  P.pos++;
  return t->ident;
}

//
// Scopes
//

int scope_level(void) {
  return depth;
}

void enter_scope(void) {
  depth++;
  vec_push(&scope_stack, NULL);
}

void leave_scope(void) {
  Binding *b = scope_stack.data[scope_stack.len - 1];
  for (; b; b = b->scope_next) {
    if (b->is_tag)
      b->ident->tag = b->shadowed;
    else
      b->ident->ordinary = b->shadowed;
  }
  scope_stack.len--;
  depth--;
}

static Binding *new_binding(Ident *id, bool is_tag) {
  Binding *b = xalloc(sizeof(Binding));
  b->ident = id;
  b->depth = depth;
  b->is_tag = is_tag;
  b->shadowed = is_tag ? id->tag : id->ordinary;
  if (scope_stack.len == 0)
    vec_push(&scope_stack, NULL);
  b->scope_next = scope_stack.data[scope_stack.len - 1];
  scope_stack.data[scope_stack.len - 1] = b;
  if (is_tag)
    id->tag = b;
  else
    id->ordinary = b;
  return b;
}

void bind_ordinary(Ident *name, Sym *sym) {
  new_binding(name, false)->sym = sym;
}

static void bind_tag(Ident *name, Type *ty) {
  new_binding(name, true)->tag = ty;
}

Sym *lookup_ordinary(Ident *name) {
  return name->ordinary ? name->ordinary->sym : NULL;
}

static Binding *current_scope_binding(Ident *name) {
  Binding *b = name->ordinary;
  return (b && b->depth == depth) ? b : NULL;
}

static Type *typedef_type(Token *t) {
  if (t->kind != TK_IDENT)
    return NULL;
  Sym *s = lookup_ordinary(t->ident);
  return (s && s->kind == SYM_TYPEDEF) ? s->ty : NULL;
}

bool is_type_start(Token *t) {
  switch (t->kind) {
  case KW_VOID:
  case KW_CHAR:
  case KW_SHORT:
  case KW_INT:
  case KW_LONG:
  case KW_FLOAT:
  case KW_DOUBLE:
  case KW_SIGNED:
  case KW_UNSIGNED:
  case KW_BOOL:
  case KW_STRUCT:
  case KW_UNION:
  case KW_ENUM:
  case KW_CONST:
  case KW_VOLATILE:
  case KW_RESTRICT:
  case KW_TYPEDEF:
  case KW_EXTERN:
  case KW_STATIC:
  case KW_AUTO:
  case KW_REGISTER:
  case KW_INLINE:
  case KW_NORETURN:
  case KW_ALIGNAS:
  case KW_ATOMIC:
  case KW_COMPLEX:
  case KW_THREAD_LOCAL:
  case KW_ATTRIBUTE:
  case KW_TYPEOF:
  case KW_EXTENSION:
  case KW_BUILTIN_VA_LIST:
    return true;
  case TK_IDENT:
    return typedef_type(t) != NULL;
  default:
    return false;
  }
}

//
// Symbols
//

static void add_global(Sym *s) {
  if (program_tail)
    program_tail->next = s;
  else
    program_head = s;
  program_tail = s;
}

Sym *current_function(void) {
  return cur_fn;
}

Sym *new_local(Ident *name, Type *ty, Token *tok) {
  Sym *s = xalloc(sizeof(Sym));
  s->kind = SYM_VAR;
  s->name = name;
  s->ty = ty;
  s->tok = tok;
  s->is_local = true;
  s->align = ty->align;
  if (name)
    bind_ordinary(name, s);
  if (cur_fn)
    vec_push(&cur_fn->locals, s);
  return s;
}

// A static-storage object with internal linkage and a generated name:
// static locals, file-scope compound literals.
Sym *new_static_local(Ident *name, Type *ty, Token *tok) {
  Sym *s = xalloc(sizeof(Sym));
  s->kind = SYM_VAR;
  s->name = name;
  s->ty = ty;
  s->tok = tok;
  s->is_global = true;
  s->is_defined = true;
  s->sc = SC_STATIC;
  s->align = ty->align;
  if (cur_fn)
    s->asm_name = fmt("_%s.%s.%d", cur_fn->name->name, name ? name->name : "anon", anon_counter++);
  else
    s->asm_name = fmt("l_.compoundliteral.%d", anon_counter++);
  add_global(s);
  return s;
}

Sym *new_string_sym(Token *str) {
  Sym *s = xalloc(sizeof(Sym));
  s->kind = SYM_VAR;
  s->ty = str->ty;
  s->tok = str;
  s->is_global = true;
  s->is_defined = true;
  s->is_string = true;
  s->sc = SC_STATIC;
  s->align = str->ty->base->align;
  s->asm_name = fmt("l_.str.%d", anon_counter++);
  s->data = str->str;
  add_global(s);
  return s;
}

Sym *func_name_sym(Token *tok) {
  if (!cur_fn)
    return NULL;
  // __func__ behaves as `static const char __func__[] = "name";`
  // declared at the start of the function body (C11 6.4.2.2).
  char *name = cur_fn->name->name;
  Token *t = copy_token(tok);
  t->str = name;
  t->str_bytes = strlen(name) + 1;
  t->ty = array_of(qualified(ty_char, Q_CONST), t->str_bytes);
  return new_string_sym(t);
}

Type *va_list_type(void) {
  // Apple's arm64 va_list is a plain pointer into the argument area.
  return pointer_to(ty_char);
}

//
// Attributes
//

typedef struct {
  StorageClass sc;
  bool is_inline;
  bool is_noreturn;
  int align;
  bool packed;
  bool used_attr;
  char *asm_label;
  Token *sc_tok;
} DeclAttr;

// __attribute__((...)): `aligned`, `packed` and `noreturn` matter, the
// rest is accepted and ignored.
static void attributes(DeclAttr *attr) {
  while (peek()->kind == KW_ATTRIBUTE) {
    Token *at = next_tok();
    expect('(', "after __attribute__");
    expect('(', "after __attribute__");
    while (!accept(')')) {
      Token *name = next_tok();
      if (name->kind == TK_EOF)
        error_tok(at, "unterminated attribute list");
      if (name->kind == ',')
        continue;
      char *n = xstrndup(name->text, name->len);
      if (strncmp(n, "__", 2) == 0 && str_endswith(n, "__") && strlen(n) > 4)
        n = xstrndup(n + 2, strlen(n) - 4);
      if (peek()->kind == '(') {
        if (str_eq(n, "aligned") && attr) {
          next_tok();
          attr->align = (int)parse_const_int();
          expect(')', "after attribute argument");
          continue;
        }
        int d = 0;
        do {
          Token *t = next_tok();
          if (t->kind == '(')
            d++;
          else if (t->kind == ')')
            d--;
          else if (t->kind == TK_EOF)
            error_tok(at, "unterminated attribute");
        } while (d > 0);
        continue;
      }
      if (!attr)
        continue;
      if (str_eq(n, "packed"))
        attr->packed = true;
      else if (str_eq(n, "aligned"))
        attr->align = 16;
      else if (str_eq(n, "noreturn"))
        attr->is_noreturn = true;
      else if (str_eq(n, "used"))
        attr->used_attr = true;
    }
    expect(')', "to close __attribute__");
  }
}

// Attributes and an asm label after a declarator.
static void declarator_suffix(DeclAttr *attr) {
  for (;;) {
    if (peek()->kind == KW_ATTRIBUTE) {
      attributes(attr);
      continue;
    }
    if (peek()->kind == KW_ASM) {
      Token *t = next_tok();
      expect('(', "after asm");
      Token *s = peek();
      if (s->kind != TK_STR)
        error_tok(t, "expected a string literal naming the symbol");
      next_tok();
      expect(')', "after asm label");
      if (attr)
        attr->asm_label = xstrdup(s->str);
      continue;
    }
    return;
  }
}

//
// Declaration specifiers
//

static Type *struct_or_union(void);
static Type *enum_specifier(void);

static Type *typeof_specifier(void) {
  Token *t = next_tok();
  expect('(', "after typeof");
  Type *ty;
  if (is_type_start(peek())) {
    ty = parse_typename();
  } else {
    Expr *e = parse_expr();
    ty = e->ty;
  }
  expect(')', "after typeof operand");
  (void)t;
  return ty;
}

static Type *declspec(void *attr_arg) {
  DeclAttr *attr = attr_arg;
  enum { VOID, BOOL, CHAR, SHORT, INT, LONG, FLOAT, DOUBLE, SIGNED, UNSIGNED, NCOUNT };
  int count[NCOUNT];
  memset(count, 0, sizeof(count));
  int nbasic = 0;
  Type *other = NULL;
  int qual = 0;
  Token *start = peek();

  for (;;) {
    Token *t = peek();
    switch (t->kind) {
    case KW_TYPEDEF:
    case KW_EXTERN:
    case KW_STATIC:
    case KW_AUTO:
    case KW_REGISTER: {
      if (!attr)
        error_tok(t, "storage class specifier is not allowed here");
      if (attr->sc != SC_NONE)
        error_tok(t, "cannot combine with previous '%.*s' declaration specifier",
                  attr->sc_tok->len, attr->sc_tok->text);
      attr->sc = t->kind == KW_TYPEDEF  ? SC_TYPEDEF
                 : t->kind == KW_EXTERN ? SC_EXTERN
                 : t->kind == KW_STATIC ? SC_STATIC
                 : t->kind == KW_AUTO   ? SC_AUTO
                                        : SC_REGISTER;
      attr->sc_tok = t;
      next_tok();
      continue;
    }
    case KW_INLINE:
      if (attr)
        attr->is_inline = true;
      next_tok();
      continue;
    case KW_NORETURN:
      if (attr)
        attr->is_noreturn = true;
      next_tok();
      continue;
    case KW_CONST:
      qual |= Q_CONST;
      next_tok();
      continue;
    case KW_VOLATILE:
      qual |= Q_VOLATILE;
      next_tok();
      continue;
    case KW_RESTRICT:
      qual |= Q_RESTRICT;
      next_tok();
      continue;
    case KW_EXTENSION:
      next_tok();
      continue;
    case KW_ATOMIC:
      next_tok();
      if (peek()->kind == '(') {
        // _Atomic(T): accepted as plain T (no atomics are generated).
        next_tok();
        other = parse_typename();
        expect(')', "after _Atomic type");
        nbasic++;
      }
      continue;
    case KW_ALIGNAS: {
      next_tok();
      expect('(', "after _Alignas");
      int a;
      if (is_type_start(peek()))
        a = parse_typename()->align;
      else
        a = (int)parse_const_int();
      expect(')', "after _Alignas argument");
      if (a && !is_pow2(a))
        error_tok(t, "requested alignment is not a power of 2");
      if (!attr)
        error_tok(t, "_Alignas is not allowed here");
      if (a > attr->align)
        attr->align = a;
      continue;
    }
    case KW_ATTRIBUTE: {
      DeclAttr dummy;
      memset(&dummy, 0, sizeof(dummy));
      attributes(attr ? attr : &dummy);
      continue;
    }
    case KW_THREAD_LOCAL:
      error_tok(t, "thread-local storage is not supported");
    case KW_COMPLEX:
      error_tok(t, "complex types are not supported");
    case KW_VOID:
      count[VOID]++;
      break;
    case KW_BOOL:
      count[BOOL]++;
      break;
    case KW_CHAR:
      count[CHAR]++;
      break;
    case KW_SHORT:
      count[SHORT]++;
      break;
    case KW_INT:
      count[INT]++;
      break;
    case KW_LONG:
      count[LONG]++;
      break;
    case KW_FLOAT:
      count[FLOAT]++;
      break;
    case KW_DOUBLE:
      count[DOUBLE]++;
      break;
    case KW_SIGNED:
      count[SIGNED]++;
      break;
    case KW_UNSIGNED:
      count[UNSIGNED]++;
      break;
    case KW_STRUCT:
    case KW_UNION:
      if (other || nbasic)
        error_tok(t, "cannot combine with previous type specifier");
      other = struct_or_union();
      nbasic++;
      continue;
    case KW_ENUM:
      if (other || nbasic)
        error_tok(t, "cannot combine with previous type specifier");
      other = enum_specifier();
      nbasic++;
      continue;
    case KW_TYPEOF:
      if (other || nbasic)
        error_tok(t, "cannot combine with previous type specifier");
      other = typeof_specifier();
      nbasic++;
      continue;
    case KW_BUILTIN_VA_LIST:
      if (other || nbasic)
        error_tok(t, "cannot combine with previous type specifier");
      next_tok();
      other = va_list_type();
      nbasic++;
      continue;
    case TK_IDENT: {
      // A typedef name is a type specifier only if no other type
      // specifier has been seen: in `long T;` T is being declared.
      Type *td = (nbasic == 0) ? typedef_type(t) : NULL;
      if (!td)
        goto done;
      next_tok();
      other = td;
      nbasic++;
      continue;
    }
    default:
      goto done;
    }
    if (other)
      error_tok(t, "cannot combine with previous type specifier");
    nbasic++;
    next_tok();
  }
done:;
  Type *ty;
  if (other) {
    ty = other;
  } else if (nbasic == 0) {
    // Implicit int (C89).  Accepted with a warning for old code.
    if (!attr || (attr->sc == SC_NONE && !attr->is_inline && !qual))
      error_tok(start, "expected a type specifier, found %s", tok_desc(start));
    warn_tok(start, "type specifier missing, defaults to 'int'");
    ty = ty_int;
  } else {
    int nsign = count[SIGNED] + count[UNSIGNED];
    bool bad = nsign > 1 || count[VOID] > 1 || count[BOOL] > 1 || count[CHAR] > 1 ||
               count[SHORT] > 1 || count[INT] > 1 || count[LONG] > 2 || count[FLOAT] > 1 ||
               count[DOUBLE] > 1;
    bool u = count[UNSIGNED] > 0;
    if (count[VOID]) {
      bad |= nbasic != 1;
      ty = ty_void;
    } else if (count[BOOL]) {
      bad |= nbasic != 1;
      ty = ty_bool;
    } else if (count[CHAR]) {
      bad |= nbasic != 1 + nsign;
      ty = count[UNSIGNED] ? ty_uchar : count[SIGNED] ? ty_schar : ty_char;
    } else if (count[FLOAT]) {
      bad |= nbasic != 1;
      ty = ty_float;
    } else if (count[DOUBLE]) {
      bad |= nbasic != 1 + count[LONG] || count[LONG] > 1;
      ty = count[LONG] ? ty_ldouble : ty_double;
    } else if (count[SHORT]) {
      bad |= count[LONG] > 0;
      ty = u ? ty_ushort : ty_short;
    } else if (count[LONG] == 2) {
      ty = u ? ty_ullong : ty_llong;
    } else if (count[LONG] == 1) {
      ty = u ? ty_ulong : ty_long;
    } else {
      ty = u ? ty_uint : ty_int;
    }
    if (bad)
      error_tok(start, "invalid combination of type specifiers");
  }
  return qualified(ty, qual);
}

//
// Declarators
//

typedef struct {
  Type *ty;
  Token *name;
  Token *tok; // where the declarator starts
} Declarator;

static Type *func_params(Type *ret);

static int64_t array_len(void) {
  // [ static? qualifiers* expr? ] or [*]
  while (peek()->kind == KW_STATIC || peek()->kind == KW_CONST || peek()->kind == KW_VOLATILE ||
         peek()->kind == KW_RESTRICT)
    next_tok();
  if (accept(']'))
    return -1;
  if (peek()->kind == '*' && peek_at(1)->kind == ']') {
    // [*]: a variable length array of unspecified size, only valid in a
    // prototype, where the parameter becomes a pointer anyway.
    next_tok();
    next_tok();
    return -1;
  }
  Token *t = peek();
  Expr *e = parse_assign();
  int64_t n;
  if (!is_integer(e->ty))
    error_tok(t, "size of array has non-integer type '%s'", type_str(e->ty));
  if (!eval_int_const(e, &n))
    error_tok(t, "variable length arrays are not supported");
  if (n < 0 && !e->ty->is_unsigned)
    error_tok(t, "array has negative size");
  expect(']', "after array size");
  return n;
}

// Applies a sequence of array and function suffixes to `ty`.  They are
// read left to right but bind right to left: int a[2][3] is an array of
// two arrays of three ints.
static Type *type_suffixes(Type *ty) {
  if (peek()->kind == '[') {
    Token *t = next_tok();
    int64_t n = array_len();
    Type *inner = type_suffixes(ty);
    if (inner->kind == TY_FUNC)
      error_tok(t, "'%s' declared as array of functions", type_str(inner));
    if (inner->kind == TY_VOID)
      error_tok(t, "array has incomplete element type 'void'");
    if (!is_complete(inner) && !(is_record(inner) || inner->kind == TY_ENUM))
      error_tok(t, "array has incomplete element type '%s'", type_str(inner));
    return array_of(inner, n);
  }
  if (peek()->kind == '(') {
    Token *t = next_tok();
    Type *fn = func_params(ty);
    if (ty->kind == TY_FUNC)
      error_tok(t, "function cannot return function type");
    if (ty->kind == TY_ARRAY)
      error_tok(t, "function cannot return array type");
    // Further suffixes would apply to the return type: f()() is an error.
    if (peek()->kind == '(' || peek()->kind == '[')
      error_tok(peek(), "function cannot return %s type",
                peek()->kind == '(' ? "function" : "array");
    return fn;
  }
  return ty;
}

static Type *pointers(Type *ty) {
  while (accept('*')) {
    ty = pointer_to(ty);
    int q = 0;
    for (;;) {
      Token *t = peek();
      if (t->kind == KW_CONST)
        q |= Q_CONST;
      else if (t->kind == KW_VOLATILE)
        q |= Q_VOLATILE;
      else if (t->kind == KW_RESTRICT)
        q |= Q_RESTRICT;
      else if (t->kind == KW_ATOMIC || t->kind == KW_EXTENSION)
        ;
      else if (t->kind == KW_ATTRIBUTE) {
        attributes(NULL);
        continue;
      } else
        break;
      next_tok();
    }
    ty = qualified(ty, q);
  }
  return ty;
}

enum { NAME_REQUIRED, NAME_OPTIONAL, NAME_FORBIDDEN };

// Whether the '(' at the current position starts a nested declarator
// rather than a parameter list.  Where a name is required, a direct
// declarator can only start with the name or '('.  Elsewhere a '(' that
// is followed by a type or ')' begins a parameter list (C11 6.7.6.3p11:
// a parenthesised typedef name in a parameter is a function type).
static bool nested_declarator_follows(int mode) {
  if (mode == NAME_REQUIRED)
    return true;
  int k = 1;
  Token *t = peek_at(k);
  // Look past attributes: (__attribute__((x)) *p) is still nested.
  while (t->kind == KW_ATTRIBUTE) {
    t = peek_at(++k);
    int depth = 0;
    for (;;) {
      if (t->kind == '(')
        depth++;
      else if (t->kind == ')' && --depth == 0)
        break;
      else if (t->kind == TK_EOF)
        return false;
      t = peek_at(++k);
    }
    t = peek_at(++k);
  }
  return !(is_type_start(t) || t->kind == ')');
}

// declarator = pointers ( "(" declarator ")" | ident )? suffixes
//
// In `int (*fp)[3]` the nested declarator `*fp` applies to the type
// built by the suffixes that follow it.  So the nested part is skipped,
// the suffixes are parsed, and then the nested part is parsed again
// with that type as its base.
static Declarator declarator(Type *ty, int mode) {
  Declarator d;
  memset(&d, 0, sizeof(d));
  d.tok = peek();
  attributes(NULL);
  ty = pointers(ty);

  if (peek()->kind == '(' && nested_declarator_follows(mode)) {
    Token *open = next_tok();
    int inner_start = P.pos;
    for (int level = 1; level > 0;) {
      Token *t = next_tok();
      if (t->kind == TK_EOF)
        error_tok(open, "expected ')' to close declarator");
      if (t->kind == '(')
        level++;
      else if (t->kind == ')')
        level--;
    }
    Type *outer = type_suffixes(ty);
    int after = P.pos;
    P.pos = inner_start;
    Declarator inner = declarator(outer, mode);
    expect(')', "to close declarator");
    P.pos = after;
    inner.tok = d.tok;
    return inner;
  }

  if (peek()->kind == TK_IDENT && mode != NAME_FORBIDDEN) {
    d.name = next_tok();
  } else if (mode == NAME_REQUIRED) {
    error_tok(peek(), "expected identifier or '(', found %s", tok_desc(peek()));
  }
  d.ty = type_suffixes(ty);
  return d;
}

// The parameter list after '(' (the '(' is consumed).
static Type *func_params(Type *ret) {
  Vec types = {0};
  Vec names = {0};
  bool variadic = false;

  if (peek()->kind == KW_VOID && peek_at(1)->kind == ')') {
    next_tok();
    next_tok();
    return func_type(ret, NULL, 0, false, true);
  }
  if (accept(')'))
    return func_type(ret, NULL, 0, false, false);

  // K&R identifier list: f(a, b, c)
  if (peek()->kind == TK_IDENT && !typedef_type(peek())) {
    for (;;) {
      Token *n = peek();
      if (n->kind != TK_IDENT)
        error_tok(n, "expected parameter name");
      next_tok();
      vec_push(&types, ty_int);
      vec_push(&names, n);
      if (accept(')'))
        break;
      expect(',', "between parameter names");
    }
    Type *fn = func_type(ret, (Type **)types.data, types.len, false, false);
    fn->param_toks = (Token **)names.data;
    return fn;
  }

  enter_scope(); // function prototype scope
  for (;;) {
    if (peek()->kind == P_ELLIPSIS) {
      if (types.len == 0)
        error_tok(peek(), "ISO C requires a named parameter before '...'");
      next_tok();
      variadic = true;
      expect(')', "after '...'");
      break;
    }
    Token *start = peek();
    DeclAttr attr;
    memset(&attr, 0, sizeof(attr));
    Type *base = declspec(&attr);
    if (attr.sc != SC_NONE && attr.sc != SC_REGISTER)
      error_tok(start, "invalid storage class for a parameter");
    Declarator d = declarator(base, NAME_OPTIONAL);
    declarator_suffix(NULL);
    Type *pt = d.ty;
    // Parameter adjustments (C11 6.7.6.3p7-8).
    if (pt->kind == TY_ARRAY)
      pt = qualified(pointer_to(pt->base), 0);
    else if (pt->kind == TY_FUNC)
      pt = pointer_to(pt);
    if (pt->kind == TY_VOID)
      error_tok(start, "parameter has incomplete type 'void'");
    vec_push(&types, pt);
    vec_push(&names, d.name);
    if (accept(')'))
      break;
    expect(',', "or ')' after parameter");
  }
  leave_scope();
  Type *fn = func_type(ret, (Type **)types.data, types.len, variadic, true);
  fn->param_toks = (Token **)names.data;
  return fn;
}

Type *parse_typename(void) {
  Token *t = peek();
  Type *base = declspec(NULL);
  Declarator d = declarator(base, NAME_FORBIDDEN);
  (void)t;
  return d.ty;
}

//
// Struct, union and enum specifiers
//

static void struct_members(Type *ty) {
  Member head;
  memset(&head, 0, sizeof(head));
  Member *cur = &head;

  while (!accept('}')) {
    if (peek()->kind == KW_STATIC_ASSERT) {
      Token *t = next_tok();
      expect('(', "after _Static_assert");
      int64_t v = parse_const_int();
      char *msg = NULL;
      if (accept(','))
        msg = next_tok()->str;
      expect(')', "after _Static_assert");
      expect(';', "after _Static_assert");
      if (!v)
        error_tok(t, "static assertion failed%s%s", msg ? ": " : "", msg ? msg : "");
      continue;
    }
    if (accept(';'))
      continue;
    DeclAttr attr;
    memset(&attr, 0, sizeof(attr));
    Token *start = peek();
    Type *base = declspec(&attr);
    if (attr.sc != SC_NONE)
      error_tok(start, "storage class is not allowed in a member declaration");

    // Anonymous struct or union member (C11 6.7.2.1p13).
    if (is_record(base) && peek()->kind == ';') {
      next_tok();
      Member *m = xalloc(sizeof(Member));
      m->ty = base;
      m->tok = start;
      cur = cur->next = m;
      continue;
    }

    for (bool first = true; !accept(';'); first = false) {
      if (!first)
        expect(',', "between member declarators");
      Member *m = xalloc(sizeof(Member));
      m->tok = peek();
      if (peek()->kind == ':') {
        m->ty = base; // unnamed bit-field
      } else {
        Declarator d = declarator(base, NAME_REQUIRED);
        m->name = d.name->ident;
        m->tok = d.name;
        m->ty = d.ty;
      }
      if (accept(':')) {
        Token *wt = peek();
        int64_t w = parse_const_int();
        if (!is_integer(m->ty))
          error_tok(m->tok, "bit-field has non-integral type '%s'", type_str(m->ty));
        if (w < 0)
          error_tok(wt, "bit-field has negative width");
        if (w > m->ty->size * 8)
          error_tok(wt, "width of bit-field (%lld bits) exceeds the width of its type (%lld bits)",
                    (long long)w, (long long)(m->ty->size * 8));
        if (w == 0 && m->name)
          error_tok(wt, "named bit-field has zero width");
        m->is_bitfield = true;
        m->bit_width = (int)w;
      }
      DeclAttr mattr;
      memset(&mattr, 0, sizeof(mattr));
      declarator_suffix(&mattr);
      if (m->ty->kind == TY_FUNC)
        error_tok(m->tok, "field declared as a function");
      int a = attr.align > mattr.align ? attr.align : mattr.align;
      if (a > m->ty->align) {
        m->ty = qualified(m->ty, 0);
        Type *c = xalloc(sizeof(Type));
        *c = *m->ty;
        c->align = a;
        c->ptr_to = NULL;
        c->unqual = c;
        m->ty = c;
      }
      cur = cur->next = m;
    }
  }

  // A flexible array member is allowed only last (C11 6.7.2.1p18).
  for (Member *m = head.next; m; m = m->next) {
    if (m->ty->kind == TY_ARRAY && m->ty->len < 0) {
      if (m->next)
        error_tok(m->tok, "flexible array member must be the last member");
      if (ty->kind == TY_STRUCT) {
        ty->rec->flexible = true;
        m->ty = array_of(m->ty->base, 0);
        m->ty->size = 0;
      }
    }
  }
  ty->rec->members = head.next;
}

static Type *struct_or_union(void) {
  Token *kw = next_tok();
  TypeKind kind = kw->kind == KW_STRUCT ? TY_STRUCT : TY_UNION;
  DeclAttr attr;
  memset(&attr, 0, sizeof(attr));
  attributes(&attr);

  Token *tag_tok = NULL;
  if (peek()->kind == TK_IDENT)
    tag_tok = next_tok();
  if (!tag_tok && peek()->kind != '{')
    error_tok(peek(), "expected '{' or a tag name after '%.*s'", kw->len, kw->text);

  Type *ty = NULL;
  if (tag_tok) {
    Binding *b = tag_tok->ident->tag;
    bool defining = peek()->kind == '{';
    bool forward = peek()->kind == ';';
    if (b && (b->depth == depth || (!defining && !forward))) {
      // Refers to (or completes) the visible declaration.
      ty = b->tag;
      if (ty->kind != kind)
        error_tok(tag_tok, "use of '%s' with tag type that does not match previous declaration",
                  tag_tok->ident->name);
      if (defining && ty->rec->complete && b->depth == depth)
        error_tok(tag_tok, "redefinition of '%s %s'", kind == TY_STRUCT ? "struct" : "union",
                  tag_tok->ident->name);
    }
    if (!ty) {
      ty = new_record_type(kind, tag_tok->ident, tag_tok);
      bind_tag(tag_tok->ident, ty);
    }
  } else {
    ty = new_record_type(kind, NULL, kw);
  }

  if (!accept('{'))
    return ty;
  if (ty->rec->complete) {
    // A redefinition in an inner scope declares a new type.
    ty = new_record_type(kind, tag_tok ? tag_tok->ident : NULL, tag_tok ? tag_tok : kw);
    if (tag_tok)
      bind_tag(tag_tok->ident, ty);
  }
  struct_members(ty);
  attributes(&attr);
  ty->rec->packed = attr.packed;
  if (attr.align > ty->align)
    ty->align = attr.align;
  layout_record(ty, tag_tok ? tag_tok : kw);
  return ty;
}

static Type *enum_specifier(void) {
  Token *kw = next_tok();
  attributes(NULL);
  Token *tag_tok = NULL;
  if (peek()->kind == TK_IDENT)
    tag_tok = next_tok();
  if (!tag_tok && peek()->kind != '{')
    error_tok(peek(), "expected '{' or a tag name after 'enum'");

  Type *ty = NULL;
  if (tag_tok) {
    Binding *b = tag_tok->ident->tag;
    if (b && (b->depth == depth || peek()->kind != '{')) {
      ty = b->tag;
      if (ty->kind != TY_ENUM)
        error_tok(tag_tok, "use of '%s' with tag type that does not match previous declaration",
                  tag_tok->ident->name);
    }
    if (!ty) {
      ty = enum_type(tag_tok->ident, tag_tok);
      bind_tag(tag_tok->ident, ty);
    }
  } else {
    ty = enum_type(NULL, kw);
  }
  if (!accept('{'))
    return ty;
  if (ty->rec->complete)
    error_tok(tag_tok, "redefinition of 'enum %s'", tag_tok->ident->name);

  int64_t val = 0;
  int64_t lo = 0, hi = 0;
  Vec consts = {0};
  bool first = true;
  while (!accept('}')) {
    if (!first) {
      expect(',', "between enumerators");
      if (accept('}'))
        break;
    }
    first = false;
    Token *name = peek();
    Ident *id = expect_ident("an enumerator name");
    attributes(NULL);
    if (accept('=')) {
      Token *et = peek();
      Expr *e = parse_cond_expr();
      if (!eval_int_const(e, &val) || !is_integer(e->ty))
        error_tok(et, "expression is not an integer constant expression");
      if (e->ty->is_unsigned && e->ty->size == 4)
        val = (uint32_t)val;
    }
    Binding *prev = current_scope_binding(id);
    if (prev)
      error_tok(name, "redefinition of '%s'", id->name);
    Sym *s = xalloc(sizeof(Sym));
    s->kind = SYM_ENUMCONST;
    s->name = id;
    s->tok = name;
    s->enum_val = val;
    s->ty = (val >= INT32_MIN && val <= INT32_MAX) ? ty_int : ty_long;
    bind_ordinary(id, s);
    vec_push(&consts, s);
    if (first || val < lo)
      lo = val < lo ? val : lo;
    if (val > hi)
      hi = val;
    val++;
  }
  // The underlying type follows clang: unsigned int unless a value is
  // negative, widened to 64 bits if needed.
  Type *base;
  if (lo < 0)
    base = (lo >= INT32_MIN && hi <= INT32_MAX) ? ty_int : ty_long;
  else
    base = hi <= (int64_t)UINT32_MAX ? ty_uint : ty_ulong;
  ty->rec->enum_base = base;
  ty->size = base->size;
  ty->align = base->align;
  ty->is_unsigned = base->is_unsigned;
  ty->rec->complete = true;
  attributes(NULL);
  return ty;
}

//
// Declarations
//

static char *global_asm_name(Ident *name, DeclAttr *attr) {
  if (attr && attr->asm_label)
    return attr->asm_label;
  return fmt("_%s", name->name);
}

// Objects and functions with linkage, by name: a block-scope `extern`
// declaration and a later file-scope one must denote the same entity.
static Map linkage_map;

static Sym *declare_global(Declarator *d, DeclAttr *attr) {
  Ident *id = d->name->ident;
  Type *ty = d->ty;
  bool is_fn = ty->kind == TY_FUNC;
  Binding *b = id->ordinary;
  bool same_scope = b && b->depth == depth;

  Sym *prev = NULL;
  if (same_scope) {
    prev = b->sym;
    if (prev->kind == SYM_TYPEDEF || prev->kind == SYM_ENUMCONST || prev->is_local ||
        (prev->is_global && prev->sc == SC_STATIC && !prev->is_extern_linkage && depth > 0))
      error_tok(d->name, "redefinition of '%s' as different kind of symbol", id->name);
  }
  if (!prev)
    prev = map_get(&linkage_map, id->name);

  if (prev) {
    bool ok = types_compatible(unqual(prev->ty), unqual(ty));
    if (!ok && prev->ty->kind == TY_FUNC && ty->kind == TY_FUNC &&
        types_compatible(unqual(prev->ty->ret), unqual(ty->ret)) &&
        (!prev->ty->has_proto || !ty->has_proto))
      ok = true; // an old-style declaration and a prototype
    if (!ok)
      error_tok(d->name, "conflicting types for '%s' ('%s' vs '%s')", id->name, type_str(ty),
                type_str(prev->ty));
    // Keep the most complete type, with the parameter names of the
    // latest declaration (a definition needs its own names).
    Type *ct = composite_type(prev->ty, ty);
    if (ct->kind == TY_FUNC && ty->kind == TY_FUNC && ty->param_toks && ct != ty) {
      Type *c = xalloc(sizeof(Type));
      *c = *ct;
      c->param_toks = ty->param_toks;
      c->ptr_to = NULL;
      c->unqual = c;
      ct = c;
    }
    prev->ty = ct;
    if (attr->is_inline)
      prev->is_inline = true;
    if (attr->is_noreturn)
      prev->is_noreturn = true;
    if (attr->align > prev->align)
      prev->align = attr->align;
    if (!same_scope)
      bind_ordinary(id, prev);
    return prev;
  }

  Sym *s = xalloc(sizeof(Sym));
  s->kind = is_fn ? SYM_FUNC : SYM_VAR;
  s->name = id;
  s->ty = ty;
  s->tok = d->name;
  s->sc = attr->sc;
  s->is_global = !is_fn;
  s->is_extern_linkage = attr->sc != SC_STATIC;
  s->is_inline = attr->is_inline;
  s->is_noreturn = attr->is_noreturn;
  s->asm_name = global_asm_name(id, attr);
  s->align = attr->align > ty->align ? attr->align : ty->align;
  map_put(&linkage_map, id->name, s);
  add_global(s);
  bind_ordinary(id, s);
  return s;
}

static void check_object_type(Declarator *d, Type *ty) {
  if (ty->kind == TY_VOID)
    error_tok(d->name, "variable has incomplete type 'void'");
}

static Stmt *new_stmt(StmtKind k, Token *tok) {
  Stmt *s = xalloc(sizeof(Stmt));
  s->kind = k;
  s->tok = tok;
  return s;
}

static void function_definition(Sym *fn, Declarator *d);

// Parses an init-declarator list after the declaration specifiers.
// Returns the statements that initialise automatic variables (for a
// block-scope declaration), or NULL.
static Stmt *init_declarators(Type *base, DeclAttr *attr, Token *start) {
  Vec inits = {0};
  bool first = true;

  if (accept(';')) {
    // `struct S;`, `enum E {...};`, `int;`
    if (!is_record(base) && base->kind != TY_ENUM)
      warn_tok(start, "declaration does not declare anything");
    return NULL;
  }

  for (;; first = false) {
    if (!first)
      expect(',', "or ';' after declarator");
    DeclAttr dattr = *attr;
    Declarator d = declarator(base, NAME_REQUIRED);
    declarator_suffix(&dattr);
    Type *ty = d.ty;
    if (dattr.align && dattr.align < ty->align && !is_record(ty))
      dattr.align = ty->align;

    if (dattr.sc == SC_TYPEDEF) {
      Binding *b = current_scope_binding(d.name->ident);
      if (b && !(b->sym->kind == SYM_TYPEDEF && types_compatible(b->sym->ty, ty)))
        error_tok(d.name, "redefinition of '%s'", d.name->ident->name);
      Sym *s = xalloc(sizeof(Sym));
      s->kind = SYM_TYPEDEF;
      s->name = d.name->ident;
      s->ty = ty;
      s->tok = d.name;
      if (dattr.align > ty->align) {
        Type *c = xalloc(sizeof(Type));
        *c = *ty;
        c->align = dattr.align;
        c->ptr_to = NULL;
        c->unqual = c;
        s->ty = c;
      }
      bind_ordinary(d.name->ident, s);
      if (accept(';'))
        return NULL;
      continue;
    }

    if (ty->kind == TY_FUNC) {
      if (depth > 0 && dattr.sc == SC_STATIC)
        error_tok(d.name, "function declared in block scope cannot have 'static' storage class");
      Sym *fn = declare_global(&d, &dattr);
      if (first && depth == 0 && (peek()->kind == '{' || is_type_start(peek()))) {
        function_definition(fn, &d);
        return NULL;
      }
      if (accept(';'))
        break;
      continue;
    }

    if (depth == 0 || dattr.sc == SC_EXTERN) {
      // An object with static storage duration and linkage.
      if (depth == 0 && (dattr.sc == SC_AUTO || dattr.sc == SC_REGISTER))
        error_tok(d.name, "illegal storage class on file-scoped variable");
      Sym *s = declare_global(&d, &dattr);
      if (accept('=')) {
        if (depth > 0)
          error_tok(d.name, "declaration of block scope identifier with linkage cannot have an initializer");
        if (s->is_defined)
          error_tok(d.name, "redefinition of '%s'", s->name->name);
        if (dattr.sc == SC_EXTERN)
          warn_tok(d.name, "'extern' variable has an initializer");
        Type *completed = NULL;
        Init *init = parse_initializer(s->ty, &completed);
        if (completed)
          s->ty = completed;
        s->is_defined = true;
        s->is_tentative = false;
        if (dattr.sc == SC_STATIC)
          s->is_extern_linkage = false;
        make_static_data(s, init);
      } else if (dattr.sc != SC_EXTERN) {
        if (!s->is_defined)
          s->is_tentative = true;
      }
      if (accept(';'))
        break;
      continue;
    }

    // Block scope.
    check_object_type(&d, ty);
    Binding *b = current_scope_binding(d.name->ident);
    if (b && !(b->sym && b->sym->sc == SC_EXTERN && dattr.sc == SC_EXTERN))
      error_tok(d.name, "redefinition of '%s'", d.name->ident->name);

    if (dattr.sc == SC_STATIC) {
      Sym *s = new_static_local(d.name->ident, ty, d.name);
      bind_ordinary(d.name->ident, s);
      if (dattr.align > s->align)
        s->align = dattr.align;
      if (accept('=')) {
        Type *completed = NULL;
        Init *init = parse_initializer(ty, &completed);
        if (completed)
          s->ty = completed;
        make_static_data(s, init);
      } else if (!is_complete(ty)) {
        error_tok(d.name, "variable has incomplete type '%s'", type_str(ty));
      }
      if (accept(';'))
        break;
      continue;
    }

    Sym *s = new_local(d.name->ident, ty, d.name);
    if (dattr.align > s->align)
      s->align = dattr.align;
    if (accept('=')) {
      Type *completed = NULL;
      Init *init = parse_initializer(ty, &completed);
      if (completed)
        s->ty = completed;
      Stmt *st = new_stmt(S_DECL, d.name);
      st->var = s;
      st->vinit = init;
      vec_push(&inits, st);
    } else if (!is_complete(ty)) {
      error_tok(d.name, "variable has incomplete type '%s'", type_str(ty));
    }
    if (accept(';'))
      break;
  }

  if (inits.len == 0)
    return NULL;
  if (inits.len == 1)
    return inits.data[0];
  Stmt *blk = new_stmt(S_BLOCK, start);
  blk->stmts = (Stmt **)inits.data;
  blk->nstmts = inits.len;
  return blk;
}

static Stmt *static_assert_decl(void) {
  Token *t = next_tok();
  expect('(', "after _Static_assert");
  Token *et = peek();
  Expr *e = parse_cond_expr();
  int64_t v;
  if (!eval_int_const(e, &v))
    error_tok(et, "static assertion expression is not an integral constant expression");
  char *msg = NULL;
  if (accept(',')) {
    Token *s = expect(TK_STR, "message in _Static_assert");
    msg = s->str;
  }
  expect(')', "after _Static_assert");
  expect(';', "after _Static_assert");
  if (!v)
    error_tok(t, "static assertion failed%s%s", msg ? ": " : "", msg ? msg : "");
  return NULL;
}

// A declaration (C11 6.7) at block or file scope.
static Stmt *declaration(void) {
  if (peek()->kind == KW_STATIC_ASSERT)
    return static_assert_decl();
  Token *start = peek();
  DeclAttr attr;
  memset(&attr, 0, sizeof(attr));
  Type *base = declspec(&attr);
  return init_declarators(base, &attr, start);
}

//
// Statements
//

static Stmt *compound_stmt(void);

static Label *find_label(Ident *name, Token *tok) {
  for (int i = 0; i < labels.len; i++) {
    Label *l = labels.data[i];
    if (l->name == name)
      return l;
  }
  Label *l = xalloc(sizeof(Label));
  l->name = name;
  l->tok = tok;
  vec_push(&labels, l);
  return l;
}

static Expr *condition(const char *what) {
  expect('(', what);
  Token *t = peek();
  Expr *e = parse_expr();
  expect(')', "after condition");
  e = rvalue(e);
  if (!is_scalar(e->ty))
    error_tok(t, "statement requires expression of scalar type ('%s' invalid)", type_str(e->ty));
  return e;
}

static Stmt *statement(void) {
  Token *t = peek();
  switch (t->kind) {
  case '{':
    return compound_stmt();
  case ';':
    next_tok();
    return new_stmt(S_NULL, t);
  case KW_IF: {
    next_tok();
    Stmt *s = new_stmt(S_IF, t);
    s->expr = condition("after 'if'");
    s->then = statement();
    if (accept(KW_ELSE))
      s->els = statement();
    return s;
  }
  case KW_WHILE: {
    next_tok();
    Stmt *s = new_stmt(S_WHILE, t);
    s->expr = condition("after 'while'");
    loop_depth++;
    break_depth++;
    s->then = statement();
    loop_depth--;
    break_depth--;
    return s;
  }
  case KW_DO: {
    next_tok();
    Stmt *s = new_stmt(S_DO, t);
    loop_depth++;
    break_depth++;
    s->then = statement();
    loop_depth--;
    break_depth--;
    expect(KW_WHILE, "in do/while loop");
    s->expr = condition("after 'while'");
    expect(';', "after do/while statement");
    return s;
  }
  case KW_FOR: {
    next_tok();
    Stmt *s = new_stmt(S_FOR, t);
    expect('(', "after 'for'");
    enter_scope();
    if (is_type_start(peek()) || peek()->kind == KW_STATIC_ASSERT) {
      s->init = declaration();
    } else if (!accept(';')) {
      Stmt *e = new_stmt(S_EXPR, peek());
      e->expr = parse_expr();
      s->init = e;
      expect(';', "in 'for' statement");
    }
    if (peek()->kind != ';') {
      Token *ct = peek();
      s->expr = rvalue(parse_expr());
      if (!is_scalar(s->expr->ty))
        error_tok(ct, "statement requires expression of scalar type ('%s' invalid)",
                  type_str(s->expr->ty));
    }
    expect(';', "in 'for' statement");
    if (peek()->kind != ')')
      s->inc = parse_expr();
    expect(')', "after 'for' clauses");
    loop_depth++;
    break_depth++;
    s->then = statement();
    loop_depth--;
    break_depth--;
    leave_scope();
    return s;
  }
  case KW_SWITCH: {
    next_tok();
    Stmt *s = new_stmt(S_SWITCH, t);
    expect('(', "after 'switch'");
    Token *et = peek();
    Expr *e = rvalue(parse_expr());
    expect(')', "after switch expression");
    if (!is_integer(e->ty))
      error_tok(et, "statement requires expression of integer type ('%s' invalid)",
                type_str(e->ty));
    s->expr = cast_to(e, promote_int(e->ty));
    Stmt *saved = cur_switch;
    Vec *saved_cases = cur_cases;
    Vec cases = {0};
    cur_switch = s;
    cur_cases = &cases;
    break_depth++;
    s->then = statement();
    break_depth--;
    s->cases = (Stmt **)cases.data;
    s->ncases = cases.len;
    cur_switch = saved;
    cur_cases = saved_cases;
    return s;
  }
  case KW_CASE: {
    next_tok();
    if (!cur_switch)
      error_tok(t, "'case' statement not in switch statement");
    Stmt *s = new_stmt(S_CASE, t);
    Token *vt = peek();
    Expr *e = parse_cond_expr();
    int64_t lo, hi;
    if (!eval_int_const(e, &lo) || !is_integer(e->ty))
      error_tok(vt, "expression is not an integer constant expression");
    hi = lo;
    if (accept(P_ELLIPSIS)) { // GNU case range
      Expr *e2 = parse_cond_expr();
      if (!eval_int_const(e2, &hi))
        error_tok(vt, "expression is not an integer constant expression");
    }
    expect(':', "after 'case'");
    // Convert to the promoted type of the controlling expression.
    Type *st = cur_switch->expr->ty;
    if (st->size == 4) {
      lo = st->is_unsigned ? (int64_t)(uint32_t)lo : (int64_t)(int32_t)lo;
      hi = st->is_unsigned ? (int64_t)(uint32_t)hi : (int64_t)(int32_t)hi;
    }
    s->case_lo = lo;
    s->case_hi = hi;
    for (int i = 0; i < cur_cases->len; i++) {
      Stmt *c = cur_cases->data[i];
      if (lo <= c->case_hi && c->case_lo <= hi)
        error_tok(vt, "duplicate case value '%lld'", (long long)lo);
    }
    vec_push(cur_cases, s);
    s->then = statement();
    return s;
  }
  case KW_DEFAULT: {
    next_tok();
    if (!cur_switch)
      error_tok(t, "'default' statement not in switch statement");
    if (cur_switch->default_stmt)
      error_tok(t, "multiple default labels in one switch");
    expect(':', "after 'default'");
    Stmt *s = new_stmt(S_DEFAULT, t);
    cur_switch->default_stmt = s;
    s->then = statement();
    return s;
  }
  case KW_BREAK:
    next_tok();
    if (break_depth == 0)
      error_tok(t, "'break' statement not in loop or switch statement");
    expect(';', "after 'break'");
    return new_stmt(S_BREAK, t);
  case KW_CONTINUE:
    next_tok();
    if (loop_depth == 0)
      error_tok(t, "'continue' statement not in loop statement");
    expect(';', "after 'continue'");
    return new_stmt(S_CONTINUE, t);
  case KW_GOTO: {
    next_tok();
    if (peek()->kind == '*')
      error_tok(t, "computed goto is not supported");
    Token *nt = peek();
    Ident *id = expect_ident("a label name");
    expect(';', "after goto");
    Stmt *s = new_stmt(S_GOTO, t);
    s->label = find_label(id, nt);
    s->label->used = true;
    return s;
  }
  case KW_RETURN: {
    next_tok();
    Stmt *s = new_stmt(S_RETURN, t);
    Type *ret = cur_fn->ty->ret;
    if (!accept(';')) {
      Token *et = peek();
      Expr *e = parse_expr();
      expect(';', "after return statement");
      if (ret->kind == TY_VOID) {
        if (e->ty->kind != TY_VOID)
          error_tok(et, "void function '%s' should not return a value", cur_fn->name->name);
        s->expr = e;
      } else {
        s->expr = assign_conv(e, unqual(ret), et, "returning");
      }
    } else if (ret->kind != TY_VOID) {
      warn_tok(t, "non-void function '%s' should return a value", cur_fn->name->name);
    }
    return s;
  }
  case TK_IDENT:
    if (peek_at(1)->kind == ':') {
      next_tok();
      next_tok();
      Label *l = find_label(t->ident, t);
      if (l->defined)
        error_tok(t, "redefinition of label '%s'", t->ident->name);
      l->defined = true;
      l->tok = t;
      attributes(NULL);
      Stmt *s = new_stmt(S_LABEL, t);
      s->label = l;
      // A label must be followed by a statement; C23 allows a
      // declaration or the end of a block, accept that too.
      if (peek()->kind == '}')
        s->then = new_stmt(S_NULL, t);
      else if (is_type_start(peek())) {
        s->then = declaration();
        if (!s->then)
          s->then = new_stmt(S_NULL, t);
      }
      else
        s->then = statement();
      return s;
    }
    break;
  default:
    break;
  }
  Stmt *s = new_stmt(S_EXPR, t);
  s->expr = parse_expr();
  expect(';', "after expression");
  return s;
}

// Reads block items up to and including the closing brace.
Stmt *parse_compound_stmt_body(void) {
  Token *t = P.toks[P.pos - 1];
  Vec items = {0};
  while (!accept('}')) {
    if (peek()->kind == TK_EOF)
      error_tok(t, "expected '}' to match this '{'");
    Stmt *s;
    if ((is_type_start(peek()) && peek_at(1)->kind != ':') || peek()->kind == KW_STATIC_ASSERT)
      s = declaration();
    else
      s = statement();
    if (s)
      vec_push(&items, s);
  }
  Stmt *blk = new_stmt(S_BLOCK, t);
  blk->stmts = (Stmt **)items.data;
  blk->nstmts = items.len;
  return blk;
}

static Stmt *compound_stmt(void) {
  expect('{', "to start block");
  enter_scope();
  Stmt *s = parse_compound_stmt_body();
  leave_scope();
  return s;
}

//
// Function definitions
//

static void function_definition(Sym *fn, Declarator *d) {
  Type *ty = fn->ty;
  if (fn->is_defined)
    error_tok(d->name, "redefinition of '%s'", fn->name->name);
  fn->is_defined = true;
  fn->tok = d->name;
  Type *dty = d->ty;

  cur_fn = fn;
  fn->locals.len = 0;
  labels.len = 0;
  enter_scope();

  // K&R parameter declarations between ')' and '{'.
  bool knr = !dty->has_proto && dty->nparams > 0;
  Type **ptypes = xalloc(sizeof(Type *) * (dty->nparams + 1));
  for (int i = 0; i < dty->nparams; i++)
    ptypes[i] = dty->params[i];
  if (knr) {
    while (peek()->kind != '{') {
      Token *start = peek();
      DeclAttr attr;
      memset(&attr, 0, sizeof(attr));
      Type *base = declspec(&attr);
      for (;;) {
        Declarator pd = declarator(base, NAME_REQUIRED);
        int idx = -1;
        for (int i = 0; i < dty->nparams; i++)
          if (dty->param_toks[i]->ident == pd.name->ident)
            idx = i;
        if (idx < 0)
          error_tok(pd.name, "parameter named '%s' is missing", pd.name->ident->name);
        Type *pt = pd.ty;
        if (pt->kind == TY_ARRAY)
          pt = pointer_to(pt->base);
        else if (pt->kind == TY_FUNC)
          pt = pointer_to(pt);
        ptypes[idx] = pt;
        if (accept(';'))
          break;
        expect(',', "between parameter declarators");
      }
      (void)start;
    }
    fn->is_knr = true;
  } else if (!dty->has_proto && dty->nparams == 0 && peek()->kind != '{') {
    error_tok(peek(), "expected function body");
  }

  fn->params = xalloc(sizeof(Sym *) * (dty->nparams + 1));
  fn->nparams = dty->nparams;
  for (int i = 0; i < dty->nparams; i++) {
    Token *nt = dty->param_toks ? dty->param_toks[i] : NULL;
    if (!nt)
      error_tok(d->name, "parameter name omitted");
    Type *pt = ptypes[i];
    if (!is_complete(pt))
      error_tok(nt, "variable has incomplete type '%s'", type_str(pt));
    if (current_scope_binding(nt->ident))
      error_tok(nt, "redefinition of parameter '%s'", nt->ident->name);
    Sym *p = new_local(nt->ident, pt, nt);
    p->is_param = true;
    fn->params[i] = p;
  }
  if (knr) {
    // The function's type for its own calls: still unprototyped.
    Type *nt = func_type(ty->ret, ptypes, dty->nparams, false, false);
    nt->param_toks = dty->param_toks;
    fn->ty = nt;
  }
  if (!is_complete(ty->ret) && ty->ret->kind != TY_VOID)
    error_tok(d->name, "incomplete result type '%s' in function definition",
              type_str(ty->ret));

  expect('{', "to start function body");
  fn->body = parse_compound_stmt_body();
  leave_scope();

  for (int i = 0; i < labels.len; i++) {
    Label *l = labels.data[i];
    if (!l->defined)
      error_tok(l->tok, "use of undeclared label '%s'", l->name->name);
  }
  vec_push(&func_defs, fn);
  cur_fn = NULL;
}

//
// Translation unit
//

static void finish_globals(void) {
  for (Sym *s = program_head; s; s = s->next) {
    if (s->kind != SYM_VAR || s->is_string)
      continue;
    if (s->is_tentative && !s->is_defined) {
      // A tentative definition becomes a zero-initialised definition
      // (C11 6.9.2p2).
      if (s->ty->kind == TY_ARRAY && s->ty->len < 0) {
        warn_tok(s->tok, "tentative array definition assumed to have one element");
        s->ty = array_of(s->ty->base, 1);
      }
      if (!is_complete(s->ty))
        error_tok(s->tok, "tentative definition has type '%s' that is never completed",
                  type_str(s->ty));
      s->is_defined = true;
    }
  }
}

Program *parse_program(Token *tok) {
  Vec v = {0};
  for (Token *t = tok; t; t = t->next) {
    vec_push(&v, t);
    if (t->kind == TK_EOF)
      break;
  }
  P.toks = (Token **)v.data;
  P.pos = 0;
  depth = 0;
  vec_push(&scope_stack, NULL);

  while (peek()->kind != TK_EOF) {
    if (accept(';'))
      continue;
    if (peek()->kind == KW_ASM) {
      Token *t = next_tok();
      error_tok(t, "file-scope asm is not supported");
    }
    declaration();
  }
  finish_globals();

  Program *prog = xalloc(sizeof(Program));
  prog->syms = program_head;
  prog->funcs = func_defs;
  return prog;
}
