// The preprocessor (C11 6.10).
//
// It is a pull-based token stream.  Input comes from a stack of
// sources: files (whose tokens may contain directives) and macro
// expansions (whose tokens never do).  Asking for the next token pops
// exhausted sources, runs directives and expands macros; an expansion is
// pushed as a new source, so its tokens are rescanned together with the
// rest of the input, exactly as the standard requires (a function-like
// macro at the end of an expansion can take its arguments from the
// text that follows).
//
// Recursion is controlled with Prosser's hidesets: every token carries
// the set of macros it was produced by and is never expanded by a macro
// in that set.

#include "ouro.h"

struct Hideset {
  Hideset *next;
  Ident *name;
};

enum {
  B_NONE,
  B_FILE,
  B_LINE,
  B_COUNTER,
  B_BASE_FILE,
  B_INCLUDE_LEVEL,
  B_DATE,
  B_TIME,
};

struct Macro {
  Ident *name;
  bool funclike;
  bool variadic;
  Ident **params; // the variadic parameter, if any, is last
  int nparams;
  Token **body;
  int *argref; // per body token: parameter index, or -1
  int nbody;
  Token *def;  // name token of the definition (for diagnostics)
  int builtin;
};

typedef struct Source Source;
struct Source {
  Source *prev;
  Token *tok;     // next token
  bool is_file;
  bool barrier;   // macro argument being pre-expanded: stop at its end
  SrcFile *file;
  int cond_base;  // conditional stack depth when the file was entered
  int dir_index;  // include directory the file was found in (#include_next)
};

typedef struct CondFrame CondFrame;
struct CondFrame {
  CondFrame *prev;
  Token *tok;
  bool taken;   // some group of this #if chain has been processed
  bool in_else;
};

Vec include_dirs;
Vec quote_dirs;
bool opt_E_linemarkers = true;

static Source *src;
static CondFrame *conds;
static int cond_depth;
static int include_level;
static Map pragma_once;
static Map guard_macro; // file path -> Ident of a detected include guard
static Ident *id_defined, *id_va_args, *id_va_opt, *id_has_include, *id_has_include_next,
    *id_pragma_op, *id_has_attribute, *id_has_builtin, *id_has_feature, *id_has_extension,
    *id_has_c_attribute;

//
// Hidesets
//

static bool hs_contains(Hideset *hs, Ident *id) {
  for (; hs; hs = hs->next)
    if (hs->name == id)
      return true;
  return false;
}

static Hideset *hs_add(Hideset *hs, Ident *id) {
  if (hs_contains(hs, id))
    return hs;
  Hideset *n = xalloc(sizeof(Hideset));
  n->name = id;
  n->next = hs;
  return n;
}

static Hideset *hs_union(Hideset *a, Hideset *b) {
  for (; a; a = a->next)
    b = hs_add(b, a->name);
  return b;
}

static Hideset *hs_intersect(Hideset *a, Hideset *b) {
  Hideset *r = NULL;
  for (; a; a = a->next)
    if (hs_contains(b, a->name))
      r = hs_add(r, a->name);
  return r;
}

//
// Sources
//

static void push_source(Token *list, bool is_file, SrcFile *file) {
  Source *s = xalloc(sizeof(Source));
  s->prev = src;
  s->tok = list;
  s->is_file = is_file;
  s->file = file;
  s->cond_base = cond_depth;
  src = s;
}

static Token *new_eof(Token *tmpl) {
  Token *t = copy_token(tmpl);
  t->kind = TK_EOF;
  t->len = 0;
  return t;
}

// Collects the rest of the current directive line from the file source.
static Token *read_line(void) {
  Token head = {0};
  Token *cur = &head;
  Token *t = src->tok;
  while (t->kind != TK_EOF && !(t->flags & TF_BOL)) {
    cur = cur->next = copy_token(t);
    t = t->next;
  }
  cur->next = new_eof(t);
  src->tok = t;
  return head.next;
}

static void skip_line(Token *directive) {
  Token *t = src->tok;
  if (t->kind != TK_EOF && !(t->flags & TF_BOL))
    warn_tok(t, "extra tokens at end of #%.*s directive", directive->len, directive->text);
  while (t->kind != TK_EOF && !(t->flags & TF_BOL))
    t = t->next;
  src->tok = t;
}

//
// Macro definitions
//

static Macro *new_macro(Ident *name, Token *def) {
  Macro *m = xalloc(sizeof(Macro));
  m->name = name;
  m->def = def;
  return m;
}

static bool same_definition(Macro *a, Macro *b) {
  if (a->funclike != b->funclike || a->variadic != b->variadic || a->nparams != b->nparams ||
      a->nbody != b->nbody || a->builtin || b->builtin)
    return false;
  for (int i = 0; i < a->nparams; i++)
    if (a->params[i] != b->params[i])
      return false;
  for (int i = 0; i < a->nbody; i++) {
    Token *x = a->body[i];
    Token *y = b->body[i];
    if (x->len != y->len || memcmp(x->text, y->text, x->len) != 0)
      return false;
    if (i > 0 && (x->flags & TF_SPACE) != (y->flags & TF_SPACE))
      return false;
  }
  return true;
}

static void define_from_line(Token *name_tok, Token *t) {
  if (name_tok->kind != TK_IDENT)
    error_tok(name_tok, "macro name must be an identifier");
  if (name_tok->ident == id_defined)
    error_tok(name_tok, "'defined' cannot be used as a macro name");
  Macro *m = new_macro(name_tok->ident, name_tok);

  if (t->kind == '(' && !(t->flags & TF_SPACE)) {
    m->funclike = true;
    Vec params = {0};
    t = t->next;
    if (t->kind != ')') {
      for (;;) {
        if (t->kind == P_ELLIPSIS) {
          m->variadic = true;
          vec_push(&params, id_va_args);
          t = t->next;
          break;
        }
        if (t->kind != TK_IDENT)
          error_tok(t, "expected a parameter name in macro definition");
        for (int i = 0; i < params.len; i++)
          if (params.data[i] == t->ident)
            error_tok(t, "duplicate macro parameter '%s'", t->ident->name);
        vec_push(&params, t->ident);
        t = t->next;
        if (t->kind == P_ELLIPSIS) { // GNU named variadic parameter: args...
          m->variadic = true;
          t = t->next;
          break;
        }
        if (t->kind != ',')
          break;
        t = t->next;
      }
    }
    if (t->kind != ')')
      error_tok(t, "expected ')' in macro parameter list");
    t = t->next;
    m->params = (Ident **)params.data;
    m->nparams = params.len;
  }

  Vec body = {0};
  for (; t->kind != TK_EOF; t = t->next)
    vec_push(&body, t);
  m->body = (Token **)body.data;
  m->nbody = body.len;
  m->argref = xalloc(sizeof(int) * (m->nbody + 1));
  for (int i = 0; i < m->nbody; i++) {
    m->argref[i] = -1;
    Token *b = m->body[i];
    if (b->kind == TK_IDENT && m->funclike)
      for (int j = 0; j < m->nparams; j++)
        if (m->params[j] == b->ident)
          m->argref[i] = j;
    if (b->kind == TK_IDENT && b->ident == id_va_args && !(m->variadic && m->argref[i] >= 0))
      warn_tok(b, "__VA_ARGS__ can only appear in the expansion of a variadic macro");
  }
  if (m->nbody > 0) {
    m->body[0]->flags &= ~(TF_SPACE | TF_BOL);
    if (m->body[0]->kind == P_HASHHASH)
      error_tok(m->body[0], "'##' cannot appear at either end of a macro expansion");
    if (m->body[m->nbody - 1]->kind == P_HASHHASH)
      error_tok(m->body[m->nbody - 1], "'##' cannot appear at either end of a macro expansion");
  }
  if (m->funclike)
    for (int i = 0; i < m->nbody; i++)
      if (m->body[i]->kind == '#' && (i + 1 >= m->nbody || m->argref[i + 1] < 0))
        error_tok(m->body[i], "'#' is not followed by a macro parameter");

  Macro *old = m->name->macro;
  if (old && !same_definition(old, m)) {
    if (old->builtin)
      warn_tok(name_tok, "redefining builtin macro '%s'", m->name->name);
    else
      warn_tok(name_tok, "'%s' macro redefined", m->name->name);
  }
  m->name->macro = m;
}

//
// Stringizing and pasting
//

static Token *stringize(Token *arg, Token *where) {
  Buf b = {0};
  buf_putc(&b, '"');
  for (Token *t = arg; t; t = t->next) {
    if (t != arg && (t->flags & (TF_SPACE | TF_BOL)))
      buf_putc(&b, ' ');
    if (t->kind == TK_STR || t->kind == TK_CHAR) {
      for (int i = 0; i < t->len; i++) {
        if (t->text[i] == '\\' || t->text[i] == '"')
          buf_putc(&b, '\\');
        buf_putc(&b, t->text[i]);
      }
    } else {
      buf_put(&b, t->text, t->len);
    }
  }
  buf_puts(&b, "\"\n");
  Token *s = lex_string("<scratch>", buf_cstr(&b), where);
  s->next = NULL;
  return s;
}

static Token *paste(Token *lhs, Token *rhs, Token *where) {
  char *text = fmt("%.*s%.*s\n", lhs->len, lhs->text, rhs->len, rhs->text);
  Token *t = lex_string("<scratch>", text, where);
  if (t->kind == TK_EOF || t->next->kind != TK_EOF)
    error_tok(where, "pasting \"%.*s\" and \"%.*s\" does not give a valid preprocessing token",
              lhs->len, lhs->text, rhs->len, rhs->text);
  t->next = NULL;
  t->flags = lhs->flags & (TF_SPACE | TF_BOL);
  return t;
}

//
// Macro expansion
//

static Token *expand_next(void);
static Token *read_token(void);

typedef struct {
  Token *raw;      // tokens as written
  Token *expanded; // fully macro-replaced (computed on demand)
  bool done;
} Arg;

static Token *copy_list(Token *t) {
  Token head = {0};
  Token *cur = &head;
  for (; t; t = t->next)
    cur = cur->next = copy_token(t);
  return head.next;
}

// Fully macro-expands an argument in isolation: the expansion may not
// read past the end of the argument.
static Token *expand_arg(Token *raw, Token *where) {
  Token *sentinel = new_eof(where);
  Token *list = copy_list(raw);
  if (list) {
    Token *t = list;
    while (t->next)
      t = t->next;
    t->next = sentinel;
  } else {
    list = sentinel;
  }
  push_source(list, false, NULL);
  src->barrier = true;
  Source *mine = src;

  Token head = {0};
  Token *cur = &head;
  for (;;) {
    Token *t = expand_next();
    if (t == sentinel)
      break;
    cur = cur->next = copy_token(t);
  }
  ASSERT(src == mine);
  src = mine->prev;
  return head.next;
}

static Token *arg_expanded(Arg *a, Token *where) {
  if (!a->done) {
    a->expanded = expand_arg(a->raw, where);
    a->done = true;
  }
  return a->expanded;
}

// Placemarker: stands for an empty argument next to ##.
static Token placemarker_tok;

typedef struct {
  Token head;
  Token *tail;
} List;

static void list_add(List *l, Token *t) {
  t->next = NULL;
  l->tail->next = t;
  l->tail = t;
}

static void list_add_all(List *l, Token *t, int flags) {
  bool first = true;
  for (; t; t = t->next) {
    Token *c = copy_token(t);
    if (first) {
      c->flags = (c->flags & ~(TF_SPACE | TF_BOL)) | (flags & TF_SPACE);
      first = false;
    } else if (c->flags & TF_BOL) {
      c->flags = (c->flags & ~TF_BOL) | TF_SPACE;
    }
    list_add(l, c);
  }
}

// Replaces the parameters in the body of `m` (C11 6.10.3.1 - 6.10.3.3).
static Token *substitute(Macro *m, Arg *args, Token *invocation) {
  List out;
  memset(&out, 0, sizeof(out));
  out.tail = &out.head;

  for (int i = 0; i < m->nbody; i++) {
    Token *bt = m->body[i];
    int ref = m->argref[i];

    // # parameter
    if (m->funclike && bt->kind == '#' && i + 1 < m->nbody && m->argref[i + 1] >= 0) {
      Token *s = stringize(args[m->argref[i + 1]].raw, invocation);
      s->flags = bt->flags & TF_SPACE;
      list_add(&out, s);
      i++;
      continue;
    }

    // ## : paste the last token so far with the first of the next operand
    if (bt->kind == P_HASHHASH) {
      Token *rhs_bt = m->body[i + 1];
      int rref = m->argref[i + 1];
      Token *lhs = out.tail;

      // GNU: `, ## __VA_ARGS__` drops the comma when the variable
      // arguments are empty and otherwise pastes nothing.
      if (lhs != &out.head && lhs->kind == ',' && rref >= 0 && m->variadic &&
          rref == m->nparams - 1) {
        Token *va = args[rref].raw;
        if (!va) {
          // remove the comma
          Token *p = &out.head;
          while (p->next != lhs)
            p = p->next;
          p->next = NULL;
          out.tail = p;
        } else {
          list_add_all(&out, arg_expanded(&args[rref], invocation), rhs_bt->flags);
        }
        i++;
        continue;
      }

      Token *rhs_first;
      Token *rhs_rest = NULL;
      if (rref >= 0) {
        Token *raw = args[rref].raw;
        rhs_first = raw ? raw : &placemarker_tok;
        rhs_rest = raw ? raw->next : NULL;
      } else {
        rhs_first = rhs_bt;
      }

      if (lhs == &out.head) {
        ICE("## at start of macro body");
      } else if (lhs->kind == TK_EOF && lhs->text == NULL) {
        // the left operand was an empty argument (placemarker)
        if (rhs_first != &placemarker_tok) {
          Token *c = copy_token(rhs_first);
          c->flags = lhs->flags;
          *lhs = *c;
          lhs->next = NULL;
        }
      } else if (rhs_first != &placemarker_tok) {
        Token *p = paste(lhs, rhs_first, invocation);
        *lhs = *p;
        lhs->next = NULL;
      }
      if (rhs_rest)
        list_add_all(&out, rhs_rest, TF_SPACE & rhs_rest->flags);
      i++;
      continue;
    }

    if (ref >= 0) {
      bool next_is_paste = i + 1 < m->nbody && m->body[i + 1]->kind == P_HASHHASH;
      if (next_is_paste) {
        Token *raw = args[ref].raw;
        if (!raw) {
          Token *pm = xalloc(sizeof(Token));
          pm->kind = TK_EOF; // a placemarker: removed at the end
          pm->text = NULL;
          pm->flags = bt->flags & TF_SPACE;
          list_add(&out, pm);
        } else {
          list_add_all(&out, raw, bt->flags);
        }
      } else if (args[ref].raw == NULL && m->variadic && ref == m->nparams - 1) {
        // empty __VA_ARGS__ expands to nothing
      } else {
        list_add_all(&out, arg_expanded(&args[ref], invocation), bt->flags);
      }
      continue;
    }

    // __VA_OPT__(content): the content if the variable arguments are
    // non-empty (C23, also a common extension).
    if (bt->kind == TK_IDENT && bt->ident == id_va_opt && m->variadic && i + 1 < m->nbody &&
        m->body[i + 1]->kind == '(') {
      int depth = 0;
      int j = i + 1;
      for (; j < m->nbody; j++) {
        if (m->body[j]->kind == '(')
          depth++;
        else if (m->body[j]->kind == ')' && --depth == 0)
          break;
      }
      bool nonempty = arg_expanded(&args[m->nparams - 1], invocation) != NULL;
      if (nonempty) {
        for (int k = i + 2; k < j; k++) {
          if (m->argref[k] >= 0)
            list_add_all(&out, arg_expanded(&args[m->argref[k]], invocation), m->body[k]->flags);
          else
            list_add(&out, copy_token(m->body[k]));
        }
      }
      i = j;
      continue;
    }

    list_add(&out, copy_token(bt));
  }

  // Drop placemarkers.
  Token head = {0};
  Token *cur = &head;
  for (Token *t = out.head.next; t; t = t->next)
    if (!(t->kind == TK_EOF && t->text == NULL))
      cur = cur->next = t;
  cur->next = NULL;
  return head.next;
}

// Pushes the tokens of an expansion, marking where they came from.
static void push_expansion(Token *list, Token *invocation, Hideset *hs) {
  if (!list)
    return;
  Token *origin = invocation->origin ? invocation->origin : invocation;
  Token *last = NULL;
  for (Token *t = list; t; t = t->next) {
    t->hs = hs_union(hs, t->hs);
    t->origin = origin;
    last = t;
  }
  list->flags = (list->flags & ~(TF_SPACE | TF_BOL)) | (invocation->flags & (TF_SPACE | TF_BOL));
  (void)last;
  push_source(list, false, NULL);
}

// The next token from the sources without expanding macros, popping
// exhausted macro sources but never leaving a file or a barrier.
static Token *peek_raw(void) {
  for (;;) {
    if (src->tok)
      return src->tok;
    if (src->is_file || src->barrier)
      return src->tok;
    src = src->prev;
  }
}

static bool next_is_lparen(void) {
  for (Source *s = src; s; s = s->prev) {
    if (s->tok) {
      Token *t = s->tok;
      if (s->is_file && t->kind == '#' && (t->flags & TF_BOL))
        return false;
      return t->kind == '(';
    }
    if (s->is_file || s->barrier)
      return false;
  }
  return false;
}

// Reads the arguments of a function-like macro invocation; the opening
// parenthesis is next.  Returns the closing parenthesis.
static Token *read_args(Macro *m, Token *name, Arg **out) {
  Token *lp = read_token();
  ASSERT(lp->kind == '(');
  int cap = m->nparams > 0 ? m->nparams : 1;
  Arg *args = xalloc(sizeof(Arg) * (cap + 1));
  int nargs = 0;
  List cur;
  memset(&cur, 0, sizeof(cur));
  cur.tail = &cur.head;
  int depth = 0;
  Token *rp;

  for (;;) {
    Token *t = read_token();
    if (t->kind == TK_EOF)
      error_tok(name, "unterminated argument list invoking macro '%s'", m->name->name);
    if (t->kind == '(') {
      depth++;
    } else if (t->kind == ')') {
      if (depth == 0) {
        rp = t;
        break;
      }
      depth--;
    } else if (t->kind == ',' && depth == 0 &&
               !(m->variadic && nargs == m->nparams - 1)) {
      if (nargs + 1 >= cap + 1)
        error_tok(t, "too many arguments provided to function-like macro '%s'", m->name->name);
      args[nargs++].raw = cur.head.next;
      memset(&cur, 0, sizeof(cur));
      cur.tail = &cur.head;
      continue;
    }
    Token *c = copy_token(t);
    if (cur.tail == &cur.head)
      c->flags &= ~(TF_BOL | TF_SPACE);
    else if (c->flags & TF_BOL)
      c->flags = (c->flags & ~TF_BOL) | TF_SPACE;
    list_add(&cur, c);
  }
  args[nargs++].raw = cur.head.next;

  if (m->nparams == 0) {
    if (nargs > 1 || args[0].raw)
      error_tok(name, "too many arguments provided to function-like macro '%s'", m->name->name);
    nargs = 0;
  } else if (nargs < m->nparams) {
    // Only the variable arguments may be left out entirely.
    if (!(m->variadic && nargs == m->nparams - 1))
      error_tok(rp, "too few arguments provided to function-like macro '%s'", m->name->name);
    args[nargs++].raw = NULL;
  } else if (nargs > m->nparams) {
    error_tok(rp, "too many arguments provided to function-like macro '%s'", m->name->name);
  }
  *out = args;
  return rp;
}

static char *date_string(bool time_part) {
  static const char *mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  // A fixed date by default keeps builds reproducible; SOURCE_DATE_EPOCH
  // is honoured for builds that want a real one.
  char *e = getenv("SOURCE_DATE_EPOCH");
  long secs = e ? atol(e) : 0;
  long days = secs / 86400;
  long rem = secs % 86400;
  if (time_part)
    return fmt("\"%02ld:%02ld:%02ld\"", rem / 3600, rem / 60 % 60, rem % 60);
  // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
  long z = days + 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  long doe = z - era * 146097;
  long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long y = yoe + era * 400;
  long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  long mp = (5 * doy + 2) / 153;
  long d = doy - (153 * mp + 2) / 5 + 1;
  long m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2)
    y++;
  return fmt("\"%s %2ld %ld\"", mon[m - 1], d, y);
}

static Token *builtin_token(Macro *m, Token *t) {
  Token *origin = t->origin ? t->origin : t;
  char *text;
  switch (m->builtin) {
  case B_FILE:
  case B_BASE_FILE: {
    char *name = m->builtin == B_FILE ? origin->file->display : opt_base_file;
    Buf b = {0};
    buf_putc(&b, '"');
    for (char *p = name; *p; p++) {
      if (*p == '\\' || *p == '"')
        buf_putc(&b, '\\');
      buf_putc(&b, *p);
    }
    buf_putc(&b, '"');
    text = buf_cstr(&b);
    break;
  }
  case B_LINE:
    text = fmt("%d", origin->line + origin->file->line_delta);
    break;
  case B_COUNTER: {
    static int counter;
    text = fmt("%d", counter++);
    break;
  }
  case B_INCLUDE_LEVEL:
    text = fmt("%d", include_level);
    break;
  case B_DATE:
    text = date_string(false);
    break;
  case B_TIME:
    text = date_string(true);
    break;
  default:
    ICE("unknown builtin macro");
  }
  Token *r = lex_string("<scratch>", fmt("%s\n", text), t);
  r->next = NULL;
  r->flags = t->flags & (TF_SPACE | TF_BOL);
  r->hs = t->hs;
  return r;
}

// Handles _Pragma("...") (C11 6.10.9): the operator is removed; only
// `once` has an effect.
static void pragma_operator(Token *t) {
  Token *lp = expand_next();
  Token *s = expand_next();
  Token *rp = expand_next();
  if (lp->kind != '(' || s->kind != TK_STR || rp->kind != ')')
    error_tok(t, "_Pragma takes a parenthesized string literal");
}

// The next token with macros expanded.
static Token *expand_next(void) {
  for (;;) {
    Token *t = read_token();
    if (t->kind != TK_IDENT)
      return t;
    Macro *m = t->ident->macro;
    if (!m)
      return t;
    if (hs_contains(t->hs, t->ident) || (t->flags & TF_NOEXPAND)) {
      t->flags |= TF_NOEXPAND; // "painted blue": never expandable again
      return t;
    }
    if (m->builtin) {
      push_source(builtin_token(m, t), false, NULL);
      continue;
    }
    if (!m->funclike) {
      Token *body = substitute(m, NULL, t);
      if (!body) {
        // An empty expansion still separates the tokens around it.
        Token *nt = peek_raw();
        if (nt && (t->flags & TF_SPACE))
          nt->flags |= TF_SPACE;
        continue;
      }
      push_expansion(body, t, hs_add(t->hs, m->name));
      continue;
    }
    if (!next_is_lparen())
      return t;
    Arg *args;
    Token *rp = read_args(m, t, &args);
    Hideset *hs = hs_add(hs_intersect(t->hs, rp->hs), m->name);
    Token *body = substitute(m, args, t);
    if (!body)
      continue;
    push_expansion(body, t, hs);
  }
}

//
// #if expressions
//

typedef struct {
  int64_t v;
  bool u;
} PPVal;

static Token *ev_tok;
static int ev_skip; // > 0 while parsing an operand whose value is not used

static PPVal ev_cond(void);

static PPVal ev_primary(void) {
  Token *t = ev_tok;
  if (t->kind == '(') {
    ev_tok = t->next;
    PPVal v = ev_cond();
    while (ev_tok->kind == ',') { // the comma operator
      ev_tok = ev_tok->next;
      v = ev_cond();
    }
    if (ev_tok->kind != ')')
      error_tok(ev_tok, "expected ')' in preprocessor expression");
    ev_tok = ev_tok->next;
    return v;
  }
  if (t->kind == TK_PPNUM) {
    Token *c = copy_token(t);
    if (!convert_number(c) || c->kind != TK_INT)
      error_tok(t, "invalid integer constant in preprocessor expression");
    ev_tok = t->next;
    PPVal v = {c->ival, c->ty->is_unsigned};
    return v;
  }
  if (t->kind == TK_CHAR) {
    Token *c = copy_token(t);
    convert_char(c);
    ev_tok = t->next;
    PPVal v = {c->ival, false};
    return v;
  }
  if (t->kind == TK_IDENT) {
    // Identifiers left after macro expansion evaluate to 0 (C11 6.10.1p4).
    ev_tok = t->next;
    PPVal v = {0, false};
    return v;
  }
  error_tok(t, "invalid token in preprocessor expression");
}

static PPVal ev_unary(void) {
  Token *t = ev_tok;
  if (t->kind == '+' || t->kind == '-' || t->kind == '~' || t->kind == '!') {
    ev_tok = t->next;
    PPVal v = ev_unary();
    if (t->kind == '-')
      v.v = (int64_t)(0 - (uint64_t)v.v);
    else if (t->kind == '~')
      v.v = ~v.v;
    else if (t->kind == '!') {
      v.v = !v.v;
      v.u = false;
    }
    return v;
  }
  return ev_primary();
}

static int ev_prec(int kind) {
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
  }
  return 0;
}

static PPVal ev_binary(int min_prec) {
  PPVal l = ev_unary();
  for (;;) {
    Token *op = ev_tok;
    int prec = ev_prec(op->kind);
    if (prec == 0 || prec < min_prec)
      return l;
    ev_tok = op->next;
    bool dead = (op->kind == P_ANDAND && !l.v) || (op->kind == P_OROR && l.v);
    ev_skip += dead;
    PPVal r = ev_binary(prec + 1);
    ev_skip -= dead;
    bool u = l.u || r.u;
    uint64_t a = (uint64_t)l.v;
    uint64_t b = (uint64_t)r.v;
    PPVal res = {0, u};
    switch (op->kind) {
    case '*':
      res.v = (int64_t)(a * b);
      break;
    case '/':
    case '%':
      if (b == 0) {
        if (!ev_skip)
          error_tok(op, "division by zero in preprocessor expression");
        res.v = 0;
        break;
      }
      if (u)
        res.v = (int64_t)(op->kind == '/' ? a / b : a % b);
      else if (l.v == INT64_MIN && r.v == -1)
        res.v = op->kind == '/' ? INT64_MIN : 0;
      else
        res.v = op->kind == '/' ? l.v / r.v : l.v % r.v;
      break;
    case '+':
      res.v = (int64_t)(a + b);
      break;
    case '-':
      res.v = (int64_t)(a - b);
      break;
    case P_SHL:
      res.v = b >= 64 ? 0 : (int64_t)(a << b);
      res.u = l.u;
      break;
    case P_SHR:
      res.v = b >= 64 ? (l.u || l.v >= 0 ? 0 : -1) : (l.u ? (int64_t)(a >> b) : l.v >> b);
      res.u = l.u;
      break;
    case '<':
      res.v = u ? a < b : l.v < r.v;
      res.u = false;
      break;
    case '>':
      res.v = u ? a > b : l.v > r.v;
      res.u = false;
      break;
    case P_LE:
      res.v = u ? a <= b : l.v <= r.v;
      res.u = false;
      break;
    case P_GE:
      res.v = u ? a >= b : l.v >= r.v;
      res.u = false;
      break;
    case P_EQ:
      res.v = a == b;
      res.u = false;
      break;
    case P_NE:
      res.v = a != b;
      res.u = false;
      break;
    case '&':
      res.v = (int64_t)(a & b);
      break;
    case '^':
      res.v = (int64_t)(a ^ b);
      break;
    case '|':
      res.v = (int64_t)(a | b);
      break;
    case P_ANDAND:
      res.v = l.v && r.v;
      res.u = false;
      break;
    case P_OROR:
      res.v = l.v || r.v;
      res.u = false;
      break;
    }
    l = res;
  }
}

static PPVal ev_cond(void) {
  PPVal c = ev_binary(1);
  if (ev_tok->kind != '?')
    return c;
  Token *q = ev_tok;
  ev_tok = ev_tok->next;
  ev_skip += !c.v;
  PPVal a = ev_cond();
  ev_skip -= !c.v;
  if (ev_tok->kind != ':')
    error_tok(q, "expected ':' in preprocessor conditional expression");
  ev_tok = ev_tok->next;
  ev_skip += !!c.v;
  PPVal b = ev_cond();
  ev_skip -= !!c.v;
  PPVal r = c.v ? a : b;
  r.u = a.u || b.u;
  return r;
}

static char *find_include(char *name, bool quoted, bool next, int *found_index);

static Token *number_token(int64_t v, Token *where) {
  Token *t = lex_string("<scratch>", fmt("%lld\n", (long long)v), where);
  t->next = NULL;
  return t;
}

// Reads `(name)` or `name` after defined / __has_* operators.
static Token *operand_name(Token **pt, Token *op) {
  Token *t = *pt;
  bool paren = false;
  if (t->kind == '(') {
    paren = true;
    t = t->next;
  }
  if (t->kind != TK_IDENT)
    error_tok(op, "macro name must be an identifier");
  Token *name = t;
  t = t->next;
  if (paren) {
    // __has_attribute(x(y)) etc.: skip a balanced argument list
    if (t->kind == '(') {
      int depth = 0;
      do {
        if (t->kind == '(')
          depth++;
        else if (t->kind == ')')
          depth--;
        t = t->next;
      } while (depth > 0 && t->kind != TK_EOF);
    }
    if (t->kind != ')')
      error_tok(t, "expected ')'");
    t = t->next;
  }
  *pt = t;
  return name;
}

static bool known_builtin(Ident *id) {
  static const char *names[] = {
      "__builtin_va_start", "__builtin_va_end",   "__builtin_va_arg", "__builtin_va_copy",
      "__builtin_offsetof", "__builtin_expect",   "__builtin_trap",   "__builtin_unreachable",
      "__builtin_huge_val", "__builtin_huge_valf", "__builtin_inf",   "__builtin_inff",
      "__builtin_nan",      "__builtin_nanf",     "__builtin_constant_p",
  };
  for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
    if (str_eq(id->name, names[i]))
      return true;
  return false;
}

static int64_t eval_if(Token *directive) {
  Token *line = read_line();

  // Operators that look at macro definitions and files are applied
  // before macro expansion.
  Token head = {0};
  Token *cur = &head;
  for (Token *t = line; t->kind != TK_EOF;) {
    if (t->kind == TK_IDENT && t->ident == id_defined) {
      Token *op = t;
      t = t->next;
      Token *name = operand_name(&t, op);
      cur = cur->next = number_token(name->ident->macro != NULL, op);
      continue;
    }
    if (t->kind == TK_IDENT && (t->ident == id_has_include || t->ident == id_has_include_next)) {
      Token *op = t;
      t = t->next;
      if (t->kind != '(')
        error_tok(op, "expected '(' after %s", op->ident->name);
      t = t->next;
      char *name;
      bool quoted;
      if (t->kind == TK_STR) {
        name = xstrndup(t->text + 1, t->len - 2);
        quoted = true;
        t = t->next;
      } else if (t->kind == '<') {
        Buf b = {0};
        for (t = t->next; t->kind != '>' && t->kind != TK_EOF; t = t->next)
          buf_put(&b, t->text, t->len);
        if (t->kind != '>')
          error_tok(op, "expected '>'");
        t = t->next;
        name = buf_cstr(&b);
        quoted = false;
      } else {
        error_tok(t, "expected a header name");
      }
      if (t->kind != ')')
        error_tok(t, "expected ')'");
      t = t->next;
      int idx;
      bool found = find_include(name, quoted, op->ident == id_has_include_next, &idx) != NULL;
      cur = cur->next = number_token(found, op);
      continue;
    }
    if (t->kind == TK_IDENT &&
        (t->ident == id_has_attribute || t->ident == id_has_feature ||
         t->ident == id_has_extension || t->ident == id_has_c_attribute ||
         t->ident == id_has_builtin)) {
      Token *op = t;
      t = t->next;
      Token *name = operand_name(&t, op);
      int v = op->ident == id_has_builtin && known_builtin(name->ident);
      cur = cur->next = number_token(v, op);
      continue;
    }
    cur = cur->next = copy_token(t);
    t = t->next;
  }
  cur->next = NULL;

  Token *list = expand_arg(head.next, directive);
  Token h3 = {0};
  Token *c3 = &h3;
  for (Token *t = list; t; t = t->next)
    c3 = c3->next = t;
  c3->next = new_eof(directive);
  if (h3.next == NULL || h3.next->kind == TK_EOF)
    error_tok(directive, "#%.*s with no expression", directive->len, directive->text);

  ev_tok = h3.next;
  PPVal v = ev_cond();
  if (ev_tok->kind != TK_EOF)
    error_tok(ev_tok, "token is not a valid binary operator in a preprocessor subexpression");
  return v.v;
}

//
// #include
//

static bool file_exists(const char *path) {
  FILE *fp = fopen(path, "r");
  if (!fp)
    return false;
  fclose(fp);
  return true;
}

static char *dir_name(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash)
    return ".";
  if (slash == path)
    return "/";
  return xstrndup(path, (int)(slash - path));
}

// Search order: the including file's directory (quoted form only), then
// -iquote directories (quoted form only), then -I and system
// directories.  #include_next resumes after the directory the current
// file came from.
static char *find_include(char *name, bool quoted, bool next, int *found_index) {
  *found_index = -1;
  if (name[0] == '/')
    return file_exists(name) ? name : NULL;
  int start = 0;
  if (next) {
    Source *s = src;
    while (s && !s->is_file)
      s = s->prev;
    start = s ? s->dir_index + 1 : 0;
  } else if (quoted) {
    Source *s = src;
    while (s && !s->is_file)
      s = s->prev;
    if (s) {
      char *p = fmt("%s/%s", dir_name(s->file->path), name);
      if (file_exists(p))
        return p;
    }
    for (int i = 0; i < quote_dirs.len; i++) {
      char *p = fmt("%s/%s", (char *)quote_dirs.data[i], name);
      if (file_exists(p))
        return p;
    }
  }
  for (int i = start; i < include_dirs.len; i++) {
    IncludeDir *d = include_dirs.data[i];
    char *p = fmt("%s/%s", d->dir, name);
    if (file_exists(p)) {
      *found_index = i;
      return p;
    }
  }
  return NULL;
}

// Recognises the include-guard idiom: the whole file is one
// #ifndef X / #define X ... #endif group.  Returns X or NULL.
static Ident *detect_guard(Token *t) {
  if (!(t->kind == '#' && (t->flags & TF_BOL)))
    return NULL;
  t = t->next;
  if (!(t->kind == TK_IDENT && str_eq(t->ident->name, "ifndef")))
    return NULL;
  t = t->next;
  if (t->kind != TK_IDENT)
    return NULL;
  Ident *guard = t->ident;
  int depth = 0;
  Token *last_hash = NULL;
  for (t = t->next; t->kind != TK_EOF; t = t->next) {
    if (t->kind != '#' || !(t->flags & TF_BOL) || t->next->kind != TK_IDENT)
      continue;
    char *d = t->next->ident->name;
    if (str_eq(d, "if") || str_eq(d, "ifdef") || str_eq(d, "ifndef")) {
      depth++;
    } else if (str_eq(d, "endif")) {
      if (depth == 0) {
        last_hash = t;
        break;
      }
      depth--;
    } else if ((str_eq(d, "else") || str_eq(d, "elif")) && depth == 0) {
      return NULL;
    }
  }
  if (!last_hash)
    return NULL;
  // Nothing but the end of file may follow the closing #endif.
  t = last_hash->next->next;
  while (t->kind != TK_EOF && !(t->flags & TF_BOL))
    t = t->next;
  return t->kind == TK_EOF ? guard : NULL;
}

static void include_file(Token *directive, char *name, bool quoted, bool next) {
  int idx;
  char *path = find_include(name, quoted, next, &idx);
  if (!path)
    error_tok(directive, "'%s' file not found", name);
  if (map_get(&pragma_once, path))
    return;
  Ident *guard = map_get(&guard_macro, path);
  if (guard && guard->macro)
    return;
  if (include_level >= 200)
    error_tok(directive, "#include nested too deeply");
  SrcFile *f = load_file(path);
  if (!f)
    error_tok(directive, "cannot open '%s': %s", path, strerror(errno));
  Source *s = src;
  while (s && !s->is_file)
    s = s->prev;
  f->includer = s ? s->file : NULL;
  f->include_line = directive->line;
  Token *toks = lex_file(f);
  Ident *g = detect_guard(toks);
  if (g)
    map_put(&guard_macro, path, g);
  push_source(toks, true, f);
  src->dir_index = idx;
  include_level++;
}

static void do_include(Token *directive, bool next) {
  Token *t = src->tok;
  if (t->kind == TK_STR && !(t->flags & TF_BOL)) {
    char *name = xstrndup(t->text + 1, t->len - 2);
    src->tok = t->next;
    skip_line(directive);
    include_file(directive, name, true, next);
    return;
  }
  if (t->kind == '<' && !(t->flags & TF_BOL)) {
    Buf b = {0};
    Token *start = t;
    for (t = t->next; t->kind != '>'; t = t->next) {
      if ((t->flags & TF_BOL) || t->kind == TK_EOF)
        error_tok(start, "expected '>' to end the header name");
      if ((t->flags & TF_SPACE) && b.len)
        buf_putc(&b, ' ');
      buf_put(&b, t->text, t->len);
    }
    src->tok = t->next;
    skip_line(directive);
    include_file(directive, buf_cstr(&b), false, next);
    return;
  }
  // #include MACRO: expand, then interpret the result.
  Token *line = read_line();
  Token h = {0};
  Token *c = &h;
  for (Token *x = line; x->kind != TK_EOF; x = x->next)
    c = c->next = x;
  c->next = NULL;
  Token *e = expand_arg(h.next, directive);
  if (e && e->kind == TK_STR) {
    include_file(directive, xstrndup(e->text + 1, e->len - 2), true, next);
    return;
  }
  if (e && e->kind == '<') {
    Buf b = {0};
    for (e = e->next; e && e->kind != '>'; e = e->next) {
      if ((e->flags & TF_SPACE) && b.len)
        buf_putc(&b, ' ');
      buf_put(&b, e->text, e->len);
    }
    if (!e)
      error_tok(directive, "expected '>' to end the header name");
    include_file(directive, buf_cstr(&b), false, next);
    return;
  }
  error_tok(directive, "expected \"FILENAME\" or <FILENAME>");
}

//
// Conditional inclusion
//

static bool is_directive(Token *t, const char *name) {
  return t->kind == TK_IDENT && str_eq(t->ident->name, name);
}

// Skips a group whose condition is false.  Stops in front of the
// #elif, #else or #endif that ends it and returns that directive name.
static Token *skip_group(void) {
  int depth = 0;
  Token *t = src->tok;
  for (; t->kind != TK_EOF; t = t->next) {
    if (t->kind != '#' || !(t->flags & TF_BOL) || (t->next->flags & TF_BOL))
      continue;
    Token *d = t->next;
    if (is_directive(d, "if") || is_directive(d, "ifdef") || is_directive(d, "ifndef")) {
      depth++;
    } else if (is_directive(d, "endif")) {
      if (depth == 0) {
        src->tok = d->next;
        return d;
      }
      depth--;
    } else if ((is_directive(d, "elif") || is_directive(d, "else") ||
                is_directive(d, "elifdef") || is_directive(d, "elifndef")) &&
               depth == 0) {
      src->tok = d->next;
      return d;
    }
  }
  src->tok = t;
  return NULL;
}

static void push_cond(Token *tok, bool taken) {
  CondFrame *c = xalloc(sizeof(CondFrame));
  c->prev = conds;
  c->tok = tok;
  c->taken = taken;
  conds = c;
  cond_depth++;
}

static void pop_cond(void) {
  conds = conds->prev;
  cond_depth--;
}

static void process_conditional_chain(void);

// After a group has been skipped, handles the directive that ended it.
static void after_skip(Token *d) {
  for (;;) {
    if (!d)
      error_tok(conds->tok, "unterminated conditional directive");
    if (is_directive(d, "endif")) {
      skip_line(d);
      pop_cond();
      return;
    }
    if (is_directive(d, "else")) {
      if (conds->in_else)
        error_tok(d, "#else after #else");
      conds->in_else = true;
      skip_line(d);
      if (!conds->taken) {
        conds->taken = true;
        return;
      }
      d = skip_group();
      continue;
    }
    // #elif, #elifdef, #elifndef
    if (conds->in_else)
      error_tok(d, "#elif after #else");
    bool cond;
    if (is_directive(d, "elif")) {
      if (conds->taken) {
        read_line();
        cond = false;
      } else {
        cond = eval_if(d) != 0;
      }
    } else {
      Token *name = src->tok;
      if (name->kind != TK_IDENT || (name->flags & TF_BOL))
        error_tok(d, "macro name missing");
      src->tok = name->next;
      skip_line(d);
      cond = (name->ident->macro != NULL) == is_directive(d, "elifdef");
      if (conds->taken)
        cond = false;
    }
    if (cond) {
      conds->taken = true;
      return;
    }
    d = skip_group();
  }
}

static void process_conditional_chain(void) {
  after_skip(skip_group());
}

static void do_line(Token *directive, Token *first) {
  // #line digit-sequence "s-char-sequence"opt, or a GNU line marker.
  Token *line = read_line();
  Token h = {0};
  Token *c = &h;
  for (Token *x = line; x->kind != TK_EOF; x = x->next)
    c = c->next = x;
  c->next = NULL;
  Token *e = first ? line : expand_arg(h.next, directive);
  if (!e || e->kind != TK_PPNUM)
    error_tok(directive, "#line directive requires a positive integer argument");
  Token *num = copy_token(e);
  if (!convert_number(num) || num->kind != TK_INT)
    error_tok(e, "#line directive requires a simple digit sequence");
  char *display = NULL;
  Token *f = e->next;
  if (f && f->kind == TK_STR) {
    Token *s = copy_token(f);
    s->next = NULL;
    Token *eof = new_eof(f);
    s->next = eof;
    convert_string(s);
    display = xstrdup(s->str);
  }
  // Retarget the rest of this file's tokens to a copy of the file with
  // the new presumed name and numbering, so tokens before the directive
  // keep theirs.
  SrcFile *old = src->file;
  SrcFile *nf = xalloc(sizeof(SrcFile));
  *nf = *old;
  if (display)
    nf->display = display;
  // The line after the directive gets the given number.
  nf->line_delta = (int)num->ival - (directive->line + 1);
  for (Token *t = src->tok; t; t = t->next)
    if (t->file == old)
      t->file = nf;
  src->file = nf;
}

static void do_pragma(Token *directive) {
  Token *t = src->tok;
  if (!(t->flags & TF_BOL) && is_directive(t, "once")) {
    Source *s = src;
    map_put(&pragma_once, s->file->path, (void *)1);
    src->tok = t->next;
    skip_line(t);
    return;
  }
  // Other pragmas (pack, GCC diagnostic, clang ...) are ignored.
  (void)directive;
  read_line();
}

static void directive(Token *hash) {
  Token *d = src->tok;
  if (d->flags & TF_BOL)
    return; // null directive
  src->tok = d->next;

  if (d->kind == TK_PPNUM) { // GNU line marker: # 12 "file" flags
    src->tok = d;
    do_line(hash, d);
    return;
  }
  if (d->kind != TK_IDENT)
    error_tok(d, "invalid preprocessing directive");
  char *name = d->ident->name;

  if (str_eq(name, "define")) {
    Token *line = read_line();
    if (line->kind == TK_EOF)
      error_tok(d, "macro name missing");
    define_from_line(line, line->next);
    return;
  }
  if (str_eq(name, "undef")) {
    Token *n = src->tok;
    if (n->kind != TK_IDENT || (n->flags & TF_BOL))
      error_tok(d, "macro name must be an identifier");
    src->tok = n->next;
    skip_line(d);
    n->ident->macro = NULL;
    return;
  }
  if (str_eq(name, "include") || str_eq(name, "import")) {
    do_include(d, false);
    return;
  }
  if (str_eq(name, "include_next")) {
    do_include(d, true);
    return;
  }
  if (str_eq(name, "if")) {
    push_cond(hash, false);
    if (eval_if(d)) {
      conds->taken = true;
      return;
    }
    process_conditional_chain();
    return;
  }
  if (str_eq(name, "ifdef") || str_eq(name, "ifndef")) {
    Token *n = src->tok;
    if (n->kind != TK_IDENT || (n->flags & TF_BOL))
      error_tok(d, "macro name missing");
    src->tok = n->next;
    skip_line(d);
    bool defined = n->ident->macro != NULL;
    push_cond(hash, false);
    if (defined == str_eq(name, "ifdef")) {
      conds->taken = true;
      return;
    }
    process_conditional_chain();
    return;
  }
  if (str_eq(name, "elif") || str_eq(name, "else") || str_eq(name, "elifdef") ||
      str_eq(name, "elifndef")) {
    // We were in a group that was processed: skip to #endif.
    if (!conds || cond_depth <= src->cond_base)
      error_tok(d, "#%s without #if", name);
    if (str_eq(name, "else")) {
      if (conds->in_else)
        error_tok(d, "#else after #else");
      conds->in_else = true;
      skip_line(d);
    } else {
      if (conds->in_else)
        error_tok(d, "#%s after #else", name);
      read_line();
    }
    Token *e;
    for (;;) {
      e = skip_group();
      if (!e)
        error_tok(conds->tok, "unterminated conditional directive");
      if (is_directive(e, "endif"))
        break;
      if (is_directive(e, "else")) {
        if (conds->in_else)
          error_tok(e, "#else after #else");
        conds->in_else = true;
      }
      read_line();
    }
    skip_line(e);
    pop_cond();
    return;
  }
  if (str_eq(name, "endif")) {
    if (!conds || cond_depth <= src->cond_base)
      error_tok(d, "#endif without #if");
    skip_line(d);
    pop_cond();
    return;
  }
  if (str_eq(name, "line")) {
    do_line(d, NULL);
    return;
  }
  if (str_eq(name, "pragma")) {
    do_pragma(d);
    return;
  }
  if (str_eq(name, "error") || str_eq(name, "warning")) {
    Token *line = read_line();
    Buf b = {0};
    for (Token *t = line; t->kind != TK_EOF; t = t->next) {
      if (t != line && (t->flags & TF_SPACE))
        buf_putc(&b, ' ');
      buf_put(&b, t->text, t->len);
    }
    if (str_eq(name, "error"))
      error_tok(d, "#error %s", buf_cstr(&b));
    warn_tok(d, "#warning %s", buf_cstr(&b));
    return;
  }
  if (str_eq(name, "ident") || str_eq(name, "sccs") || str_eq(name, "assert") ||
      str_eq(name, "unassert")) {
    read_line();
    return;
  }
  error_tok(d, "invalid preprocessing directive '#%s'", name);
}

// The next token from the sources, running directives but not
// expanding macros.
static Token *read_token(void) {
  for (;;) {
    Token *t = src->tok;
    if (!t) {
      // an exhausted macro expansion
      ASSERT(!src->is_file && !src->barrier);
      src = src->prev;
      continue;
    }
    if (src->is_file) {
      if (t->kind == TK_EOF) {
        if (cond_depth > src->cond_base)
          error_tok(conds->tok, "unterminated conditional directive");
        if (!src->prev)
          return t; // end of the main file
        src = src->prev;
        include_level--;
        continue;
      }
      if (t->kind == '#' && (t->flags & TF_BOL)) {
        src->tok = t->next;
        directive(t);
        continue;
      }
      src->tok = t->next;
      return t;
    }
    if (t->kind == TK_EOF && src->barrier)
      return t; // end of a pre-expanded argument: stay put
    src->tok = t->next;
    if (t->kind == TK_IDENT && t->ident == id_pragma_op) {
      // handled by expand_next callers
    }
    return t;
  }
}

//
// Initialisation and entry points
//

static void run_text(const char *name, char *text) {
  SrcFile *f = string_file(name, text);
  f->id = -1;
  Source *saved = src;
  src = NULL;
  push_source(lex_file(f), true, f);
  for (;;) {
    Token *t = read_token();
    if (t->kind == TK_EOF)
      break;
    error_tok(t, "unexpected token in %s", name);
  }
  src = saved;
}

static void add_builtin(const char *name, int kind) {
  Ident *id = intern_cstr(name);
  Macro *m = new_macro(id, NULL);
  m->builtin = kind;
  id->macro = m;
}

void pp_init(void) {
  id_defined = intern_cstr("defined");
  id_va_args = intern_cstr("__VA_ARGS__");
  id_va_opt = intern_cstr("__VA_OPT__");
  id_has_include = intern_cstr("__has_include");
  id_has_include_next = intern_cstr("__has_include_next");
  id_has_attribute = intern_cstr("__has_attribute");
  id_has_builtin = intern_cstr("__has_builtin");
  id_has_feature = intern_cstr("__has_feature");
  id_has_extension = intern_cstr("__has_extension");
  id_has_c_attribute = intern_cstr("__has_c_attribute");
  id_pragma_op = intern_cstr("_Pragma");

  add_builtin("__FILE__", B_FILE);
  add_builtin("__LINE__", B_LINE);
  add_builtin("__COUNTER__", B_COUNTER);
  add_builtin("__BASE_FILE__", B_BASE_FILE);
  add_builtin("__INCLUDE_LEVEL__", B_INCLUDE_LEVEL);
  add_builtin("__DATE__", B_DATE);
  add_builtin("__TIME__", B_TIME);

  // The target: arm64 macOS, LP64, signed char, 8-byte long double.
  static char predefined[] =
      "#define __STDC__ 1\n"
      "#define __STDC_VERSION__ 201112L\n"
      "#define __STDC_HOSTED__ 1\n"
      "#define __STDC_UTF_16__ 1\n"
      "#define __STDC_UTF_32__ 1\n"
      "#define __STDC_NO_ATOMICS__ 1\n"
      "#define __STDC_NO_COMPLEX__ 1\n"
      "#define __STDC_NO_THREADS__ 1\n"
      "#define __STDC_NO_VLA__ 1\n"
      "#define __ouro__ 1\n"
      "#define __OURO_VERSION__ \"" OURO_VERSION "\"\n"
      "#define __APPLE__ 1\n"
      "#define __MACH__ 1\n"
      "#define __aarch64__ 1\n"
      "#define __arm64__ 1\n"
      "#define __arm64 1\n"
      "#define __AARCH64EL__ 1\n"
      "#define __LP64__ 1\n"
      "#define _LP64 1\n"
      "#define __LITTLE_ENDIAN__ 1\n"
      "#define __ORDER_LITTLE_ENDIAN__ 1234\n"
      "#define __ORDER_BIG_ENDIAN__ 4321\n"
      "#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__\n"
      "#define __CHAR_BIT__ 8\n"
      "#define __SCHAR_MAX__ 127\n"
      "#define __SHRT_MAX__ 32767\n"
      "#define __INT_MAX__ 2147483647\n"
      "#define __LONG_MAX__ 9223372036854775807L\n"
      "#define __LONG_LONG_MAX__ 9223372036854775807LL\n"
      "#define __SIZEOF_SHORT__ 2\n"
      "#define __SIZEOF_INT__ 4\n"
      "#define __SIZEOF_LONG__ 8\n"
      "#define __SIZEOF_LONG_LONG__ 8\n"
      "#define __SIZEOF_POINTER__ 8\n"
      "#define __SIZEOF_FLOAT__ 4\n"
      "#define __SIZEOF_DOUBLE__ 8\n"
      "#define __SIZEOF_LONG_DOUBLE__ 8\n"
      "#define __SIZEOF_SIZE_T__ 8\n"
      "#define __SIZEOF_PTRDIFF_T__ 8\n"
      "#define __SIZEOF_WCHAR_T__ 4\n"
      "#define __SIZE_TYPE__ unsigned long\n"
      "#define __PTRDIFF_TYPE__ long\n"
      "#define __WCHAR_TYPE__ int\n"
      "#define __INTMAX_TYPE__ long\n"
      "#define __UINTMAX_TYPE__ unsigned long\n"
      "#define __INTPTR_TYPE__ long\n"
      "#define __UINTPTR_TYPE__ unsigned long\n"
      "#define __CHAR16_TYPE__ unsigned short\n"
      "#define __CHAR32_TYPE__ unsigned int\n"
      "#define __USER_LABEL_PREFIX__ _\n"
      "#define __FLT_EVAL_METHOD__ 0\n"
      "#define __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ 110000\n";
  run_text("<built-in>", predefined);
}

void pp_define(const char *def) {
  const char *eq = strchr(def, '=');
  char *text;
  if (eq)
    text = fmt("#define %.*s %s\n", (int)(eq - def), def, eq + 1);
  else
    text = fmt("#define %s 1\n", def);
  run_text("<command line>", text);
}

void pp_undef(const char *name) {
  intern_cstr(name)->macro = NULL;
}

Token *preprocess_file(const char *path) {
  SrcFile *f = load_file(path);
  if (!f)
    fatal("cannot open '%s': %s", path, strerror(errno));
  src = NULL;
  conds = NULL;
  cond_depth = 0;
  include_level = 0;
  push_source(lex_file(f), true, f);

  Token head = {0};
  Token *cur = &head;
  for (;;) {
    Token *t = expand_next();
    if (t->kind == TK_IDENT && t->ident == id_pragma_op) {
      pragma_operator(t);
      continue;
    }
    cur = cur->next = t;
    if (t->kind == TK_EOF)
      break;
  }
  return head.next;
}

//
// -E output
//

static bool would_paste(Token *a, Token *b) {
  if (!a)
    return false;
  int ca = (unsigned char)a->text[a->len - 1];
  int cb = (unsigned char)b->text[0];
  bool word_a = isalnum(ca) || ca == '_' || ca == '$' || ca >= 0x80 || ca == '.';
  bool word_b = isalnum(cb) || cb == '_' || cb == '$' || cb >= 0x80;
  if (word_a && (word_b || cb == '.' || cb == '\'' || cb == '"'))
    return true;
  if (a->kind == TK_PPNUM && (cb == '+' || cb == '-') && strchr("eEpP", ca))
    return true;
  char buf[3] = {(char)ca, (char)cb, 0};
  static const char *pairs[] = {"++", "--", "+=", "-=", "->", "<<", ">>", "<=", ">=", "==", "!=",
                                "&&", "||", "*=", "/=", "%=", "&=", "|=", "^=", "##", "//",
                                "/*", "<:", ":>", "<%", "%>", "%:", ".."};
  for (int i = 0; i < (int)(sizeof(pairs) / sizeof(pairs[0])); i++)
    if (str_eq(buf, pairs[i]))
      return true;
  return false;
}

void print_preprocessed(FILE *out, Token *tok) {
  SrcFile *file = NULL;
  int line = 0;
  Token *prev = NULL;
  for (Token *t = tok; t && t->kind != TK_EOF; t = t->next) {
    Token *where = t->origin ? t->origin : t;
    SrcFile *f = where->file;
    int l = where->line + f->line_delta;
    bool from_macro = t->origin != NULL;
    if (!file || f->path != file->path || f->display != file->display) {
      if (prev)
        fputc('\n', out);
      if (opt_E_linemarkers)
        fprintf(out, "# %d \"%s\"\n", l, f->display);
      file = f;
      line = l;
      prev = NULL;
    } else if (l > line && (!from_macro || (t->flags & TF_BOL))) {
      if (l - line > 8 && opt_E_linemarkers) {
        fprintf(out, "\n# %d \"%s\"\n", l, f->display);
      } else {
        for (; line < l; line++)
          fputc('\n', out);
      }
      line = l;
      prev = NULL;
    }
    if (!prev && (t->flags & TF_SPACE))
      fputc(' ', out);
    else if (prev && ((t->flags & TF_SPACE) || would_paste(prev, t)))
      fputc(' ', out);
    fwrite(t->text, 1, t->len, out);
    prev = t;
  }
  fputc('\n', out);
}
