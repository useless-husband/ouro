// Memory, strings, growable buffers, a small hash table and the
// identifier intern table.

#include "ouro.h"

// Options read by several stages (set by the driver in main.c).
bool opt_dump_ir;
bool opt_dump_ir_raw;
bool opt_dump_mir;
int opt_level = 1;
char *opt_base_file;

// The arena hands out zeroed memory from large calloc'd chunks.  Large
// requests get their own block so that a big array does not waste the
// remainder of a chunk.
enum { ARENA_CHUNK = 1 << 20 };

static char *arena_ptr;
static size_t arena_left;

void *xalloc(size_t size) {
  size = (size + 15) & ~(size_t)15;
  if (size == 0)
    size = 16;
  if (size >= ARENA_CHUNK / 8) {
    void *p = calloc(1, size);
    if (!p)
      fatal("out of memory");
    return p;
  }
  if (size > arena_left) {
    arena_ptr = calloc(1, ARENA_CHUNK);
    if (!arena_ptr)
      fatal("out of memory");
    arena_left = ARENA_CHUNK;
  }
  void *p = arena_ptr;
  arena_ptr += size;
  arena_left -= size;
  return p;
}

// Returns a copy of `old` enlarged to `new_bytes`; the tail is zeroed.
void *xgrow(void *old, int old_bytes, int new_bytes) {
  void *p = xalloc(new_bytes);
  if (old_bytes > 0)
    memcpy(p, old, old_bytes);
  return p;
}

char *xstrndup(const char *s, int n) {
  char *p = xalloc(n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

char *xstrdup(const char *s) {
  return xstrndup(s, strlen(s));
}

char *fmt(const char *f, ...) {
  char small[256];
  va_list ap;
  va_start(ap, f);
  int n = vsnprintf(small, sizeof(small), f, ap);
  va_end(ap);
  if (n < (int)sizeof(small))
    return xstrndup(small, n);
  char *p = xalloc(n + 1);
  va_start(ap, f);
  vsnprintf(p, n + 1, f, ap);
  va_end(ap);
  return p;
}

bool str_eq(const char *a, const char *b) {
  return strcmp(a, b) == 0;
}

bool str_startswith(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool str_endswith(const char *s, const char *suffix) {
  int n = strlen(s);
  int m = strlen(suffix);
  return n >= m && strcmp(s + n - m, suffix) == 0;
}

//
// Buffers
//

static void buf_reserve(Buf *b, int extra) {
  if (b->len + extra + 1 <= b->cap)
    return;
  int cap = b->cap ? b->cap : 64;
  while (cap < b->len + extra + 1)
    cap *= 2;
  b->buf = xgrow(b->buf, b->len, cap);
  b->cap = cap;
}

void buf_putc(Buf *b, int c) {
  buf_reserve(b, 1);
  b->buf[b->len++] = (char)c;
}

void buf_put(Buf *b, const char *s, int n) {
  buf_reserve(b, n);
  memcpy(b->buf + b->len, s, n);
  b->len += n;
}

void buf_puts(Buf *b, const char *s) {
  buf_put(b, s, strlen(s));
}

void buf_printf(Buf *b, const char *f, ...) {
  char small[256];
  va_list ap;
  va_start(ap, f);
  int n = vsnprintf(small, sizeof(small), f, ap);
  va_end(ap);
  if (n < (int)sizeof(small)) {
    buf_put(b, small, n);
    return;
  }
  buf_reserve(b, n);
  va_start(ap, f);
  vsnprintf(b->buf + b->len, n + 1, f, ap);
  va_end(ap);
  b->len += n;
}

char *buf_cstr(Buf *b) {
  buf_reserve(b, 0);
  b->buf[b->len] = '\0';
  return b->buf;
}

void vec_push(Vec *v, void *p) {
  if (v->len == v->cap) {
    int cap = v->cap ? v->cap * 2 : 8;
    v->data = xgrow(v->data, sizeof(void *) * v->len, sizeof(void *) * cap);
    v->cap = cap;
  }
  v->data[v->len++] = p;
}

//
// Hash table with string keys (open addressing, linear probing, no
// deletion: nothing in the compiler needs it).
//

static uint32_t hash_bytes(const char *s, int n) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 16777619u;
  }
  return h;
}

static void map_resize(Map *m) {
  int cap = m->cap ? m->cap * 2 : 64;
  char **keys = xalloc(sizeof(char *) * cap);
  void **vals = xalloc(sizeof(void *) * cap);
  for (int i = 0; i < m->cap; i++) {
    if (!m->keys[i])
      continue;
    uint32_t j = hash_bytes(m->keys[i], strlen(m->keys[i])) & (cap - 1);
    while (keys[j])
      j = (j + 1) & (cap - 1);
    keys[j] = m->keys[i];
    vals[j] = m->vals[i];
  }
  m->keys = keys;
  m->vals = vals;
  m->cap = cap;
}

void *map_get(Map *m, const char *key) {
  if (!m->cap)
    return NULL;
  uint32_t j = hash_bytes(key, strlen(key)) & (m->cap - 1);
  while (m->keys[j]) {
    if (str_eq(m->keys[j], key))
      return m->vals[j];
    j = (j + 1) & (m->cap - 1);
  }
  return NULL;
}

void map_put(Map *m, const char *key, void *val) {
  if ((m->count + 1) * 2 > m->cap)
    map_resize(m);
  uint32_t j = hash_bytes(key, strlen(key)) & (m->cap - 1);
  while (m->keys[j]) {
    if (str_eq(m->keys[j], key)) {
      m->vals[j] = val;
      return;
    }
    j = (j + 1) & (m->cap - 1);
  }
  m->keys[j] = xstrdup(key);
  m->vals[j] = val;
  m->count++;
}

//
// Identifier interning
//

enum { IDENT_BUCKETS = 1 << 14 };
static Ident *ident_table[IDENT_BUCKETS];

Ident *intern(const char *s, int len) {
  uint32_t h = hash_bytes(s, len) & (IDENT_BUCKETS - 1);
  for (Ident *id = ident_table[h]; id; id = id->hash_next)
    if (id->len == len && memcmp(id->name, s, len) == 0)
      return id;
  Ident *id = xalloc(sizeof(Ident));
  id->name = xstrndup(s, len);
  id->len = len;
  id->hash_next = ident_table[h];
  ident_table[h] = id;
  return id;
}

Ident *intern_cstr(const char *s) {
  return intern(s, strlen(s));
}

int64_t align_up(int64_t n, int64_t a) {
  if (a <= 1)
    return n;
  return (n + a - 1) / a * a;
}

bool is_pow2(uint64_t v) {
  return v && !(v & (v - 1));
}

int log2_u64(uint64_t v) {
  int n = 0;
  while (v > 1) {
    v >>= 1;
    n++;
  }
  return n;
}
