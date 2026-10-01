// Diagnostics in the conventional `file:line:col: error: message` form,
// followed by the source line and a caret under the offending column.
// When the token came out of a macro expansion, the message points at
// the macro use and a note shows the token inside the macro definition.

#include "ouro.h"

bool opt_warnings = true;
bool opt_werror;
int error_count;
int warning_count;

void fatal(const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  fprintf(stderr, "ouro: error: ");
  vfprintf(stderr, f, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

void internal_error(const char *file, int line, const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  fprintf(stderr, "ouro: internal compiler error (%s:%d): ", file, line);
  vfprintf(stderr, f, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(70);
}

// Prints `path:line:col: severity: message`, the source line and a caret.
static void report(SrcFile *file, char *loc, int line, const char *severity, const char *msg) {
  if (!file || !loc) {
    fprintf(stderr, "ouro: %s: %s\n", severity, msg);
    return;
  }
  char *start = loc;
  while (start > file->text && start[-1] != '\n')
    start--;
  char *end = loc;
  while (*end && *end != '\n')
    end++;
  int col = (int)(loc - start) + 1;

  if (file->includer && !str_eq(severity, "note"))
    fprintf(stderr, "In file included from %s:%d:\n", file->includer->display,
            file->include_line + file->includer->line_delta);
  fprintf(stderr, "%s:%d:%d: %s: %s\n", file->display, line + file->line_delta, col, severity,
          msg);

  // Tabs are echoed as tabs so that the caret lines up whatever the
  // terminal's tab width is.
  fprintf(stderr, "%5d | %.*s\n      | ", line + file->line_delta, (int)(end - start), start);
  for (char *p = start; p < loc; p++)
    fputc(*p == '\t' ? '\t' : ' ', stderr);
  fprintf(stderr, "^\n");
}

static void report_tok(Token *tok, const char *severity, const char *msg) {
  Token *where = tok->origin ? tok->origin : tok;
  report(where->file, where->loc, where->line, severity, msg);
  // The token's own position is inside the macro definition, unless it
  // was synthesised by # or ## (those live in a scratch buffer).
  if (tok->origin && tok->file && tok->file->id > 0) {
    char *note = fmt("expanded from macro '%.*s'", tok->origin->len, tok->origin->text);
    report(tok->file, tok->loc, tok->line, "note", note);
  }
}

static char *vformat(const char *f, va_list ap) {
  char small[512];
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(small, sizeof(small), f, ap);
  if (n < (int)sizeof(small)) {
    va_end(ap2);
    return xstrdup(small);
  }
  char *p = xalloc(n + 1);
  vsnprintf(p, n + 1, f, ap2);
  va_end(ap2);
  return p;
}

void error_tok(Token *tok, const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  char *msg = vformat(f, ap);
  va_end(ap);
  report_tok(tok, "error", msg);
  exit(1);
}

void error_at(SrcFile *file, char *loc, int line, const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  char *msg = vformat(f, ap);
  va_end(ap);
  report(file, loc, line, "error", msg);
  exit(1);
}

void warn_tok(Token *tok, const char *f, ...) {
  if (!opt_warnings)
    return;
  va_list ap;
  va_start(ap, f);
  char *msg = vformat(f, ap);
  va_end(ap);
  report_tok(tok, opt_werror ? "error" : "warning", msg);
  warning_count++;
  if (opt_werror)
    exit(1);
}

void note_tok(Token *tok, const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  char *msg = vformat(f, ap);
  va_end(ap);
  Token *where = tok->origin ? tok->origin : tok;
  report(where->file, where->loc, where->line, "note", msg);
}
