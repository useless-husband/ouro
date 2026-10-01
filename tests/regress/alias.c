// Memory accesses that alias in ways a load-forwarding pass must
// respect: two pointers to the same object, a pointer into a global,
// overlapping accesses of different sizes, unions, and narrow stores
// read back with either signedness.
#include <stdio.h>
#include <string.h>

int g[4] = {1, 2, 3, 4};
static long gl;

static int through(int *p, int *q) {
  *p = 10;
  *q = 20;   // p and q may be the same pointer
  return *p; // must be reloaded
}

static int global_via_ptr(int *p) {
  int before = g[1];
  *p = 99;   // may point into g
  return before + g[1];
}

static long sizes(void) {
  union { long l; unsigned char b[8]; } u;
  memset(&u, 0, sizeof u);
  long *l = &u.l;
  unsigned char *b = u.b;
  *l = 0x0102030405060708L;
  b[0] = 0xFF;  // overlaps the long
  return *l;
}

static int narrow(void) {
  union { unsigned char u; signed char s; int i; } x;
  x.i = 0;
  x.u = 200;
  int a = x.s;    // same byte, read as signed
  int b = x.u;
  x.s = -1;
  return a * 1000 + b + x.u;
}

static long distinct(long *p) {
  gl = 5;
  *p = 6;         // p may point to gl
  return gl;
}

struct pair { int a, b; };
static int fields(struct pair *s, int *alias) {
  s->a = 1;
  s->b = 2;
  *alias = 7;     // may be &s->b
  return s->a + s->b;
}

int main(void) {
  int x = 0;
  printf("%d %d\n", through(&x, &x), through(&g[0], &g[1]));
  printf("%d %d\n", global_via_ptr(&g[1]), global_via_ptr(&x));
  printf("%lx\n", sizes());
  printf("%d\n", narrow());
  long other = 0;
  printf("%ld %ld\n", distinct(&gl), distinct(&other));
  struct pair s;
  printf("%d %d\n", fields(&s, &s.b), fields(&s, &x));
  return 0;
}
