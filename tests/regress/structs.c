// Structs and unions: layout, copies, passing and returning by value,
// bit-fields, nested and anonymous members.
#include <stdio.h>
#include <stddef.h>
#include <string.h>

struct Small { char c; int i; };
struct Pair { long a, b; };
struct Big { long v[5]; };
struct Mixed { char c; double d; short s; };
struct HFA { float x, y, z; };
struct HFA2 { double a, b; };
struct Bits { unsigned a : 3; signed b : 5; unsigned c : 7; int d : 1; unsigned long e : 40; };
union U { int i; float f; char bytes[4]; };
struct Outer { int tag; union { int iv; double dv; }; struct { short x, y; } pt; };

static struct Pair make_pair(long a, long b) { struct Pair p = {a, b}; return p; }
static struct Big make_big(long base) {
  struct Big b;
  for (int i = 0; i < 5; i++) b.v[i] = base + i;
  return b;
}
static long sum_big(struct Big b) { long s = 0; for (int i = 0; i < 5; i++) s += b.v[i]; return s; }
static struct HFA scale(struct HFA h, float k) { h.x *= k; h.y *= k; h.z *= k; return h; }
static double dot(struct HFA2 a, struct HFA2 b) { return a.a * b.a + a.b * b.b; }
static int small_sum(struct Small s) { return s.c + s.i; }
static struct Mixed mixed(void) { struct Mixed m = {'m', 2.5, -7}; return m; }

int main(void) {
  printf("%zu %zu %zu %zu\n", sizeof(struct Small), sizeof(struct Mixed), sizeof(struct Bits),
         offsetof(struct Mixed, s));
  printf("%zu %zu\n", sizeof(union U), sizeof(struct Outer));
  struct Pair p = make_pair(3, 4);
  printf("%ld %ld\n", p.a, p.b);
  struct Big b = make_big(10);
  printf("%ld %ld\n", b.v[4], sum_big(b));
  struct HFA h = {1, 2, 3};
  struct HFA h2 = scale(h, 2.5f);
  printf("%g %g %g / %g\n", h2.x, h2.y, h2.z, h.x);
  struct HFA2 u = {1.5, 2}, v = {4, -1};
  printf("%g\n", dot(u, v));
  struct Small sm = {'A', 1000};
  printf("%d\n", small_sum(sm));
  struct Mixed m = mixed();
  printf("%c %g %d\n", m.c, m.d, m.s);

  struct Bits bits = {0};
  bits.a = 9; bits.b = -3; bits.c = 100; bits.d = -1; bits.e = 0x123456789AUL;
  printf("%u %d %u %d %lx\n", bits.a, bits.b, bits.c, bits.d, (unsigned long)bits.e);
  bits.a++;
  bits.b -= 20;
  printf("%u %d\n", bits.a, bits.b);

  union U un;
  un.f = 1.0f;
  printf("%x %d\n", un.i, un.bytes[3]);
  struct Outer o;
  memset(&o, 0, sizeof o);
  o.tag = 1; o.dv = 0.5; o.pt.x = 3; o.pt.y = -4;
  printf("%d %g %d %d\n", o.tag, o.dv, o.pt.x, o.pt.y);

  struct Pair arr[3] = {{1, 2}, {3, 4}, {5, 6}};
  struct Pair *pp = &arr[1];
  pp->a += 10;
  printf("%ld %ld %ld\n", arr[1].a, (pp + 1)->b, pp[-1].a);
  struct Pair copy = arr[2];
  copy.b = 99;
  printf("%ld %ld\n", arr[2].b, copy.b);
  return 0;
}
