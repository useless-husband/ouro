// The lexer turns a source file into a list of preprocessing tokens.
//
// Translation phases 1 and 2 (line endings, backslash-newline splices)
// are applied to the whole file before tokenizing.  Numbers are lexed
// as pp-numbers and literals keep their spelling: the preprocessor must
// be able to stringify and paste them before they mean anything.  The
// conversion to typed values happens after preprocessing.

#include "ouro.h"

static int next_file_id = 1;

// Normalises line endings to \n and removes backslash-newline pairs.
// The newlines removed by a splice are re-inserted at the end of the
// logical line, so every following line keeps its physical number.
static void phase12(char *p) {
  char *out = p;
  int pending = 0;
  while (*p) {
    if (p[0] == '\r') {
      p += (p[1] == '\n') ? 2 : 1;
      *out++ = '\n';
      for (; pending > 0; pending--)
        *out++ = '\n';
      continue;
    }
    if (p[0] == '\\' && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n'))) {
      p += (p[1] == '\n') ? 2 : 3;
      pending++;
      continue;
    }
    if (p[0] == '\n') {
      *out++ = *p++;
      for (; pending > 0; pending--)
        *out++ = '\n';
      continue;
    }
    *out++ = *p++;
  }
  for (; pending > 0; pending--)
    *out++ = '\n';
  *out = '\0';
}

static char *slurp(const char *path, int *len_out) {
  FILE *fp = str_eq(path, "-") ? stdin : fopen(path, "rb");
  if (!fp)
    return NULL;
  Buf b = {0};
  char chunk[8192];
  for (;;) {
    int n = fread(chunk, 1, sizeof(chunk), fp);
    if (n <= 0)
      break;
    buf_put(&b, chunk, n);
  }
  if (fp != stdin)
    fclose(fp);
  // Guarantee a final newline so that directives and // comments always
  // end with one.
  if (b.len == 0 || b.buf[b.len - 1] != '\n')
    buf_putc(&b, '\n');
  *len_out = b.len;
  return buf_cstr(&b);
}

SrcFile *load_file(const char *path) {
  int len;
  char *text = slurp(path, &len);
  if (!text)
    return NULL;
  if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
      (unsigned char)text[2] == 0xBF)
    text += 3; // UTF-8 byte order mark
  phase12(text);
  SrcFile *f = xalloc(sizeof(SrcFile));
  f->path = xstrdup(path);
  f->display = f->path;
  f->text = text;
  f->id = next_file_id++;
  return f;
}

// A file that does not exist on disk: command-line macros and the text
// produced by # and ##.  Its id is 0 so diagnostics never quote it.
SrcFile *string_file(const char *name, char *text) {
  SrcFile *f = xalloc(sizeof(SrcFile));
  f->path = xstrdup(name);
  f->display = f->path;
  f->text = text;
  f->id = 0;
  return f;
}

//
// Token construction
//

static Token *make_token(int kind, SrcFile *file, char *start, char *end, int line) {
  Token *t = xalloc(sizeof(Token));
  t->kind = kind;
  t->file = file;
  t->loc = start;
  t->text = start;
  t->len = (int)(end - start);
  t->line = line;
  return t;
}

Token *new_token(int kind, Token *tmpl) {
  Token *t = xalloc(sizeof(Token));
  *t = *tmpl;
  t->kind = kind;
  t->next = NULL;
  return t;
}

Token *copy_token(Token *t) {
  Token *c = xalloc(sizeof(Token));
  *c = *t;
  c->next = NULL;
  return c;
}

char *tok_str(Token *t) {
  return xstrndup(t->text, t->len);
}

static bool is_ident_start(int c) {
  return isalpha(c) || c == '_' || c == '$' || c >= 0x80;
}

static bool is_ident_char(int c) {
  return isalnum(c) || c == '_' || c == '$' || c >= 0x80;
}

typedef struct {
  const char *spelling;
  int kind;
} Punct;

// Longest spellings first.  Digraphs map to the token they stand for.
static const Punct puncts[] = {
    {"%:%:", P_HASHHASH}, {"<<=", P_SHL_EQ}, {">>=", P_SHR_EQ}, {"...", P_ELLIPSIS},
    {"->", P_ARROW},      {"++", P_INC},     {"--", P_DEC},     {"<<", P_SHL},
    {">>", P_SHR},        {"<=", P_LE},      {">=", P_GE},      {"==", P_EQ},
    {"!=", P_NE},         {"&&", P_ANDAND},  {"||", P_OROR},    {"*=", P_MUL_EQ},
    {"/=", P_DIV_EQ},     {"%=", P_MOD_EQ},  {"+=", P_ADD_EQ},  {"-=", P_SUB_EQ},
    {"&=", P_AND_EQ},     {"^=", P_XOR_EQ},  {"|=", P_OR_EQ},   {"##", P_HASHHASH},
    {"<:", '['},          {":>", ']'},       {"<%", '{'},       {"%>", '}'},
    {"%:", '#'},
};

static int read_punct(char *p, int *kind) {
  for (int i = 0; i < (int)(sizeof(puncts) / sizeof(puncts[0])); i++) {
    int n = strlen(puncts[i].spelling);
    if (strncmp(p, puncts[i].spelling, n) == 0) {
      *kind = puncts[i].kind;
      return n;
    }
  }
  if (strchr("!%&()*+,-./:;<=>?[]^{|}~#", *p)) {
    *kind = (unsigned char)*p;
    return 1;
  }
  return 0;
}

// Skips a character or string literal body; `p` points just after the
// opening quote.  Returns a pointer just past the closing quote.
static char *skip_quoted(SrcFile *f, char *start, char *p, int quote, int line) {
  for (;;) {
    if (*p == '\n' || *p == '\0')
      error_at(f, start, line, quote == '"' ? "missing terminating '\"' character"
                                            : "missing terminating ' character");
    if (*p == '\\') {
      p += 2;
      continue;
    }
    if (*p == quote)
      return p + 1;
    p++;
  }
}

// Returns the number of prefix characters (u8, u, U, L) before a quote,
// or -1 if `p` does not start a character or string literal.
static int literal_prefix(char *p) {
  if (*p == '"' || *p == '\'')
    return 0;
  if (p[0] == 'u' && p[1] == '8' && (p[2] == '"' || p[2] == '\''))
    return 2;
  if ((p[0] == 'u' || p[0] == 'U' || p[0] == 'L') && (p[1] == '"' || p[1] == '\''))
    return 1;
  return -1;
}

static Token *tokenize(SrcFile *f, char *p, int line) {
  Token head = {0};
  Token *cur = &head;
  bool bol = true;
  bool space = false;

  while (*p) {
    if (*p == '\n') {
      p++;
      line++;
      bol = true;
      space = false;
      continue;
    }
    if (*p == ' ' || *p == '\t' || *p == '\f' || *p == '\v' || *p == '\r') {
      p++;
      space = true;
      continue;
    }
    if (p[0] == '/' && p[1] == '/') {
      while (*p != '\n' && *p)
        p++;
      space = true;
      continue;
    }
    if (p[0] == '/' && p[1] == '*') {
      char *start = p;
      int start_line = line;
      p += 2;
      while (!(p[0] == '*' && p[1] == '/')) {
        if (!*p)
          error_at(f, start, start_line, "unterminated comment");
        if (*p == '\n')
          line++;
        p++;
      }
      p += 2;
      space = true;
      continue;
    }

    char *start = p;
    int kind;
    int pre = literal_prefix(p);
    if (pre >= 0) {
      int quote = p[pre];
      p = skip_quoted(f, start, p + pre + 1, quote, line);
      kind = quote == '"' ? TK_STR : TK_CHAR;
    } else if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
      p++;
      for (;;) {
        if ((p[0] == 'e' || p[0] == 'E' || p[0] == 'p' || p[0] == 'P') &&
            (p[1] == '+' || p[1] == '-'))
          p += 2;
        else if (isalnum((unsigned char)*p) || *p == '.' || *p == '_')
          p++;
        else
          break;
      }
      kind = TK_PPNUM;
    } else if (is_ident_start((unsigned char)*p)) {
      while (is_ident_char((unsigned char)*p))
        p++;
      kind = TK_IDENT;
    } else {
      int n = read_punct(p, &kind);
      if (n == 0) {
        n = 1;
        kind = TK_OTHER;
      }
      p += n;
    }

    Token *t = make_token(kind, f, start, p, line);
    if (kind == TK_IDENT)
      t->ident = intern(start, (int)(p - start));
    t->flags = (bol ? TF_BOL : 0) | (space ? TF_SPACE : 0);
    bol = false;
    space = false;
    cur = cur->next = t;
  }

  Token *eof = make_token(TK_EOF, f, p, p, line);
  eof->flags = TF_BOL;
  cur->next = eof;
  return head.next;
}

Token *lex_file(SrcFile *f) {
  return tokenize(f, f->text, 1);
}

// Lexes `text` as a fresh source.  The tokens take the position of
// `pos_tmpl` (if given) so that diagnostics refer to a real place.
Token *lex_string(const char *name, char *text, Token *pos_tmpl) {
  SrcFile *f = string_file(name, text);
  Token *list = tokenize(f, text, 1);
  if (pos_tmpl)
    for (Token *t = list; t; t = t->next) {
      t->line = pos_tmpl->line;
      t->origin = pos_tmpl->origin ? pos_tmpl->origin : pos_tmpl;
    }
  return list;
}

const char *kind_name(int kind) {
  static char one[2];
  switch (kind) {
  case TK_EOF:
    return "end of file";
  case TK_IDENT:
    return "identifier";
  case TK_PPNUM:
  case TK_INT:
  case TK_FLOAT:
    return "number";
  case TK_CHAR:
    return "character constant";
  case TK_STR:
    return "string literal";
  }
  if (kind < 128) {
    one[0] = (char)kind;
    return one;
  }
  for (int i = 0; i < (int)(sizeof(puncts) / sizeof(puncts[0])); i++)
    if (puncts[i].kind == kind)
      return puncts[i].spelling;
  return "token";
}

//
// Literal conversion (after preprocessing)
//

static int hexval(int c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return c - 'A' + 10;
}

// Reads one escape sequence; `p` points at the backslash.
static uint32_t read_escape(Token *t, char **pp, bool *is_ucn) {
  char *p = *pp + 1;
  *is_ucn = false;
  uint32_t c;
  if (*p >= '0' && *p <= '7') {
    c = 0;
    for (int i = 0; i < 3 && *p >= '0' && *p <= '7'; i++)
      c = c * 8 + (*p++ - '0');
    *pp = p;
    return c;
  }
  if (*p == 'x') {
    p++;
    if (!isxdigit((unsigned char)*p))
      error_tok(t, "\\x used with no following hex digits");
    c = 0;
    for (; isxdigit((unsigned char)*p); p++)
      c = (c << 4) | hexval(*p);
    *pp = p;
    return c;
  }
  if (*p == 'u' || *p == 'U') {
    int n = *p == 'u' ? 4 : 8;
    p++;
    c = 0;
    for (int i = 0; i < n; i++, p++) {
      if (!isxdigit((unsigned char)*p))
        error_tok(t, "incomplete universal character name");
      c = (c << 4) | hexval(*p);
    }
    *pp = p;
    *is_ucn = true;
    return c;
  }
  *pp = p + 1;
  switch (*p) {
  case 'a':
    return 7;
  case 'b':
    return 8;
  case 'f':
    return 12;
  case 'n':
    return 10;
  case 'r':
    return 13;
  case 't':
    return 9;
  case 'v':
    return 11;
  case 'e':
  case 'E':
    return 27; // GNU extension
  case '\\':
  case '\'':
  case '"':
  case '?':
    return *p;
  default:
    warn_tok(t, "unknown escape sequence '\\%c'", *p);
    return (unsigned char)*p;
  }
}

static uint32_t decode_utf8(Token *t, char **pp) {
  unsigned char *p = (unsigned char *)*pp;
  if (p[0] < 0x80) {
    *pp += 1;
    return p[0];
  }
  int n;
  uint32_t c;
  if ((p[0] & 0xE0) == 0xC0) {
    n = 2;
    c = p[0] & 0x1F;
  } else if ((p[0] & 0xF0) == 0xE0) {
    n = 3;
    c = p[0] & 0x0F;
  } else if ((p[0] & 0xF8) == 0xF0) {
    n = 4;
    c = p[0] & 0x07;
  } else {
    error_tok(t, "invalid UTF-8 sequence");
  }
  for (int i = 1; i < n; i++) {
    if ((p[i] & 0xC0) != 0x80)
      error_tok(t, "invalid UTF-8 sequence");
    c = (c << 6) | (p[i] & 0x3F);
  }
  *pp += n;
  return c;
}

static int encode_utf8(char *out, uint32_t c) {
  if (c < 0x80) {
    out[0] = (char)c;
    return 1;
  }
  if (c < 0x800) {
    out[0] = (char)(0xC0 | (c >> 6));
    out[1] = (char)(0x80 | (c & 0x3F));
    return 2;
  }
  if (c < 0x10000) {
    out[0] = (char)(0xE0 | (c >> 12));
    out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[2] = (char)(0x80 | (c & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (c >> 18));
  out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
  out[3] = (char)(0x80 | (c & 0x3F));
  return 4;
}

static int prefix_width(Token *t) {
  int pre = literal_prefix(t->text);
  if (pre == 2)
    return 1; // u8
  if (pre == 1)
    return t->text[0] == 'u' ? 2 : 4;
  return 1;
}

static Type *elem_type_for(Token *t, int width) {
  if (width == 1)
    return ty_char;
  if (width == 2)
    return ty_ushort;
  if (t->text[0] == 'L')
    return ty_int; // wchar_t is a signed 32-bit int on Darwin
  return ty_uint;
}

// Decodes the body of a string literal into elements of `width` bytes,
// appending to `out` (without terminator).  Returns the element count.
static int decode_string(Token *t, int width, Buf *out) {
  int pre = literal_prefix(t->text);
  char *p = t->text + pre + 1;
  char *end = t->text + t->len - 1;
  int count = 0;
  while (p < end) {
    uint32_t c;
    bool ucn = false;
    bool escaped = *p == '\\';
    if (escaped)
      c = read_escape(t, &p, &ucn);
    else if (width == 1) {
      c = (unsigned char)*p++;
    } else
      c = decode_utf8(t, &p);

    if (width == 1) {
      if (ucn) {
        char tmp[4];
        int n = encode_utf8(tmp, c);
        buf_put(out, tmp, n);
        count += n;
      } else {
        buf_putc(out, (int)(c & 0xFF));
        count++;
      }
    } else if (width == 2) {
      if (c >= 0x10000 && (!escaped || ucn)) {
        c -= 0x10000;
        uint16_t hi = (uint16_t)(0xD800 + (c >> 10));
        uint16_t lo = (uint16_t)(0xDC00 + (c & 0x3FF));
        buf_put(out, (char *)&hi, 2);
        buf_put(out, (char *)&lo, 2);
        count += 2;
      } else {
        uint16_t v = (uint16_t)c;
        buf_put(out, (char *)&v, 2);
        count++;
      }
    } else {
      buf_put(out, (char *)&c, 4);
      count++;
    }
  }
  return count;
}

// Converts a run of adjacent string literal tokens starting at `t` into
// one literal (C11 5.1.1.2 phase 6) and returns it; the following
// literals are unlinked.
static void join_strings(Token *t) {
  int width = 1;
  Token *prefixed = NULL;
  for (Token *s = t; s->kind == TK_STR; s = s->next) {
    int w = prefix_width(s);
    int pre = literal_prefix(s->text);
    if (pre > 0 && !(pre == 2)) {
      if (prefixed && (prefixed->text[0] != s->text[0]))
        error_tok(s, "unsupported concatenation of string literals with different prefixes");
      prefixed = s;
    }
    if (w > width)
      width = w;
  }
  Buf b = {0};
  int count = 0;
  Token *s = t;
  for (; s->kind == TK_STR; s = s->next)
    count += decode_string(s, width, &b);
  for (int i = 0; i < width; i++)
    buf_putc(&b, 0);
  t->next = s;
  t->str = b.buf;
  t->str_bytes = (count + 1) * width;
  t->ty = array_of(elem_type_for(prefixed ? prefixed : t, width), count + 1);
}

void convert_string(Token *t) {
  join_strings(t);
}

void convert_char(Token *t) {
  int pre = literal_prefix(t->text);
  int width = prefix_width(t);
  char *p = t->text + pre + 1;
  char *end = t->text + t->len - 1;
  if (p == end)
    error_tok(t, "empty character constant");
  int64_t val = 0;
  int n = 0;
  while (p < end) {
    uint32_t c;
    bool ucn = false;
    if (*p == '\\')
      c = read_escape(t, &p, &ucn);
    else if (width == 1)
      c = (unsigned char)*p++; // plain and u8 constants work on bytes
    else
      c = decode_utf8(t, &p);
    if (width == 1) {
      // A multi-character constant such as 'ab' is an int made of its
      // bytes, first character most significant (as clang does).
      if (ucn && c > 0x7F) {
        char tmp[4];
        int k = encode_utf8(tmp, c);
        for (int i = 0; i < k; i++)
          val = (val << 8) | (unsigned char)tmp[i];
        n += k;
        continue;
      }
      val = (val << 8) | (c & 0xFF);
    } else {
      val = c;
    }
    n++;
  }
  if (width == 1 && pre == 0) {
    t->ty = ty_int;
    t->ival = n == 1 ? (int64_t)(signed char)val : (int64_t)(int32_t)val; // char is signed
    if (n > 4)
      warn_tok(t, "character constant too long for its type");
  } else if (pre == 2) {
    t->ty = ty_uchar;
    t->ival = val & 0xFF;
  } else if (width == 2) {
    t->ty = ty_ushort;
    t->ival = val & 0xFFFF;
  } else if (t->text[0] == 'L') {
    t->ty = ty_int;
    t->ival = (int32_t)val;
  } else {
    t->ty = ty_uint;
    t->ival = (uint32_t)val;
  }
  t->kind = TK_INT;
}

// Integer constants (C11 6.4.4.1).  Returns false if the spelling is not
// an integer constant (it may still be a floating constant).
static bool convert_int(Token *t) {
  char *p = t->text;
  char *end = t->text + t->len;
  int base = 10;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
    base = 16;
    p += 2;
  } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && (p[2] == '0' || p[2] == '1')) {
    base = 2;
    p += 2;
  } else if (p[0] == '0') {
    base = 8;
  }

  uint64_t val = 0;
  bool overflow = false;
  char *q = p;
  for (; q < end; q++) {
    int d;
    if (isdigit((unsigned char)*q))
      d = *q - '0';
    else if (base == 16 && isxdigit((unsigned char)*q))
      d = hexval(*q);
    else
      break;
    if (d >= base)
      return false;
    if (val > (UINT64_MAX - d) / base)
      overflow = true;
    val = val * base + d;
  }
  if (q == p && base != 8)
    return false;

  // Suffix: any combination of one u and one l/ll.
  bool u = false;
  int l = 0;
  while (q < end) {
    if ((*q == 'u' || *q == 'U') && !u) {
      u = true;
      q++;
    } else if ((q[0] == 'l' && q + 1 < end && q[1] == 'l') ||
               (q[0] == 'L' && q + 1 < end && q[1] == 'L')) {
      if (l)
        return false;
      l = 2;
      q += 2;
    } else if ((*q == 'l' || *q == 'L') && !l) {
      l = 1;
      q++;
    } else {
      return false;
    }
  }
  if (overflow)
    warn_tok(t, "integer constant is too large for its type");

  // The first type in the list that can represent the value.
  Type *ty;
  bool fits_int = val <= 0x7FFFFFFF;
  bool fits_uint = val <= 0xFFFFFFFF;
  bool fits_long = val <= 0x7FFFFFFFFFFFFFFF;
  if (base == 10) {
    if (u)
      ty = (l == 0 && fits_uint) ? ty_uint : (l == 2 ? ty_ullong : ty_ulong);
    else if (l == 0 && fits_int)
      ty = ty_int;
    else if (l <= 1 && fits_long)
      ty = ty_long;
    else if (fits_long)
      ty = ty_llong;
    else
      ty = (l == 2) ? ty_ullong : ty_ulong; // too large for any signed type
  } else {
    if (u)
      ty = (l == 0 && fits_uint) ? ty_uint : (l == 2 ? ty_ullong : ty_ulong);
    else if (l == 0 && fits_int)
      ty = ty_int;
    else if (l == 0 && fits_uint)
      ty = ty_uint;
    else if (l <= 1 && fits_long)
      ty = ty_long;
    else if (l <= 1)
      ty = ty_ulong;
    else if (fits_long)
      ty = ty_llong;
    else
      ty = ty_ullong;
  }
  t->kind = TK_INT;
  t->ival = (int64_t)val;
  t->ty = ty;
  return true;
}

bool convert_number(Token *t) {
  if (convert_int(t))
    return true;
  char *s = tok_str(t);
  char *end;
  double v = strtod(s, &end);
  Type *ty = ty_double;
  if (*end == 'f' || *end == 'F') {
    ty = ty_float;
    v = (float)v;
    end++;
  } else if (*end == 'l' || *end == 'L') {
    ty = ty_ldouble;
    end++;
  }
  if (*end != '\0' || end == s)
    return false;
  t->kind = TK_FLOAT;
  t->fval = v;
  t->ty = ty;
  return true;
}

static void init_keywords(void) {
  static bool done;
  if (done)
    return;
  done = true;
  static const struct {
    const char *name;
    int kind;
  } kws[] = {
      {"auto", KW_AUTO},
      {"break", KW_BREAK},
      {"case", KW_CASE},
      {"char", KW_CHAR},
      {"const", KW_CONST},
      {"__const", KW_CONST},
      {"__const__", KW_CONST},
      {"continue", KW_CONTINUE},
      {"default", KW_DEFAULT},
      {"do", KW_DO},
      {"double", KW_DOUBLE},
      {"else", KW_ELSE},
      {"enum", KW_ENUM},
      {"extern", KW_EXTERN},
      {"float", KW_FLOAT},
      {"for", KW_FOR},
      {"goto", KW_GOTO},
      {"if", KW_IF},
      {"inline", KW_INLINE},
      {"__inline", KW_INLINE},
      {"__inline__", KW_INLINE},
      {"int", KW_INT},
      {"long", KW_LONG},
      {"register", KW_REGISTER},
      {"restrict", KW_RESTRICT},
      {"__restrict", KW_RESTRICT},
      {"__restrict__", KW_RESTRICT},
      {"return", KW_RETURN},
      {"short", KW_SHORT},
      {"signed", KW_SIGNED},
      {"__signed", KW_SIGNED},
      {"__signed__", KW_SIGNED},
      {"sizeof", KW_SIZEOF},
      {"static", KW_STATIC},
      {"struct", KW_STRUCT},
      {"switch", KW_SWITCH},
      {"typedef", KW_TYPEDEF},
      {"union", KW_UNION},
      {"unsigned", KW_UNSIGNED},
      {"void", KW_VOID},
      {"volatile", KW_VOLATILE},
      {"__volatile", KW_VOLATILE},
      {"__volatile__", KW_VOLATILE},
      {"while", KW_WHILE},
      {"_Alignas", KW_ALIGNAS},
      {"_Alignof", KW_ALIGNOF},
      {"__alignof", KW_ALIGNOF},
      {"__alignof__", KW_ALIGNOF},
      {"_Atomic", KW_ATOMIC},
      {"_Bool", KW_BOOL},
      {"_Complex", KW_COMPLEX},
      {"__complex__", KW_COMPLEX},
      {"_Generic", KW_GENERIC},
      {"_Imaginary", KW_IMAGINARY},
      {"_Noreturn", KW_NORETURN},
      {"_Static_assert", KW_STATIC_ASSERT},
      {"_Thread_local", KW_THREAD_LOCAL},
      {"__thread", KW_THREAD_LOCAL},
      {"asm", KW_ASM},
      {"__asm", KW_ASM},
      {"__asm__", KW_ASM},
      {"__attribute__", KW_ATTRIBUTE},
      {"__attribute", KW_ATTRIBUTE},
      {"typeof", KW_TYPEOF},
      {"__typeof", KW_TYPEOF},
      {"__typeof__", KW_TYPEOF},
      {"__extension__", KW_EXTENSION},
      {"__builtin_va_list", KW_BUILTIN_VA_LIST},
  };
  for (int i = 0; i < (int)(sizeof(kws) / sizeof(kws[0])); i++)
    intern_cstr(kws[i].name)->kw = kws[i].kind;
}

// Phase 7 entry: identifiers that spell keywords become keywords and
// literals get their values.  Adjacent string literals are joined.
void convert_keywords_and_literals(Token *t) {
  init_keywords();
  for (; t->kind != TK_EOF; t = t->next) {
    switch (t->kind) {
    case TK_IDENT:
      if (t->ident->kw)
        t->kind = t->ident->kw;
      break;
    case TK_PPNUM:
      if (!convert_number(t))
        error_tok(t, "invalid numeric constant '%.*s'", t->len, t->text);
      break;
    case TK_CHAR:
      convert_char(t);
      break;
    case TK_STR:
      convert_string(t);
      break;
    case TK_OTHER:
      error_tok(t, "stray '%.*s' in program", t->len, t->text);
    default:
      break;
    }
  }
}
