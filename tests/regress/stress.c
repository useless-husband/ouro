// Paths that small programs do not reach: frames larger than the
// immediate offset range, many values live across calls (forcing
// spills), switches with negative and 64-bit cases, nested designators,
// sizeof with side effects, goto into nested blocks, large struct copies
// and edge-case conversions.
#include <stdio.h>
#include <string.h>
#include <limits.h>

static long sink(long x) { return x ^ 0x55; }

static long big_frame(int k) {
  char big[100000];
  long words[5000];
  memset(big, k, sizeof big);
  for (int i = 0; i < 5000; i++)
    words[i] = i * k;
  big[99999] = 7;
  return big[99999] + big[50000] + words[4999] + words[0];
}

static long pressure(long a, long b) {
  long v0 = a + 1, v1 = b + 2, v2 = a * 3, v3 = b * 5, v4 = a - b, v5 = a ^ b, v6 = a | 9,
       v7 = b & 77, v8 = a + b, v9 = a - 7, v10 = b - 11, v11 = a * b, v12 = (long)((unsigned long)a << 2),
       v13 = b >> 1, v14 = a + 100, v15 = b + 200, v16 = a * 7, v17 = b * 9, v18 = a - 1,
       v19 = b - 3;
  long s = sink(v0);
  s += sink(v1) + sink(v2) + sink(v3);
  s += v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + v12 + v13 + v14 + v15 +
       v16 + v17 + v18 + v19;
  double d0 = a * 0.5, d1 = b * 0.25, d2 = a + 0.125, d3 = b - 1.5, d4 = a * 1.5, d5 = b / 2.0,
         d6 = a - 0.75, d7 = b + 3.0, d8 = a * 2.5, d9 = b * 3.5;
  s += sink((long)d0);
  s += (long)(d0 + d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8 + d9);
  return s;
}

static int sw(long v) {
  switch (v) {
  case -5: return 1;
  case -4: return 2;
  case -3: return 3;
  case -2: return 4;
  case -1: return 5;
  case 0: return 6;
  case 1: return 7;
  case 4000000000L: return 8;
  case -4000000000L: return 9;
  case LONG_MAX: return 10;
  default: return 0;
  }
}

static int sw_dense(unsigned char c) {
  switch (c) {
  case 'a': case 'e': case 'i': case 'o': case 'u': return 1;
  case 'b': case 'c': case 'd': return 2;
  case 'x': case 'y': case 'z': return 3;
  case 200: return 4;
  default: return 0;
  }
}

struct In { int a, b; int arr[3]; };
struct Out { int tag; struct In in; struct In more[2]; };

struct Big { long w[40]; };
static struct Big make_big(long base) {
  struct Big b;
  for (int i = 0; i < 40; i++) b.w[i] = base + i;
  return b;
}

static int goto_into(int n) {
  int r = 0;
  if (n > 2) goto inside;
  r = 100;
  {
    int x;
  inside:
    x = n * 2;
    r += x;
  }
  return r;
}

int main(void) {
  printf("%ld %ld\n", big_frame(3), big_frame(-2));
  printf("%ld %ld\n", pressure(10, 20), pressure(-7, 1234567));
  long vals[] = {-6, -5, -3, 0, 1, 2, 4000000000L, -4000000000L, LONG_MAX, LONG_MIN};
  for (int i = 0; i < 10; i++) printf("%d ", sw(vals[i]));
  printf("\n");
  const char *s = "abcdexyz";
  for (const char *p = s; *p; p++) printf("%d", sw_dense((unsigned char)*p));
  printf("%d\n", sw_dense(200));

  struct Out o = {.in.arr[1] = 5, 6, .tag = 1, .more[1].b = 9, .more[0] = {1, 2, {3}}};
  printf("%d %d %d %d %d %d %d\n", o.tag, o.in.arr[1], o.in.arr[2], o.more[1].b, o.more[0].a,
         o.more[0].arr[0], o.in.a);
  int i = 5;
  size_t sz = sizeof(i++) + sizeof(int[3]);
  printf("%zu %d\n", sz, i);
  struct Big b1 = make_big(100), b2;
  b2 = b1;
  b2.w[39] += 1;
  printf("%ld %ld %ld\n", b1.w[0], b1.w[39], b2.w[39]);
  printf("%d %d\n", goto_into(1), goto_into(5));
  unsigned long ul = 18446744073709551615UL;
  float f = (float)ul;
  double big = 9.2e18;
  printf("%.1f %lu %lu\n", f, (unsigned long)big, (unsigned long)(float)4294967296.0);
  unsigned u = 4294967295u;
  printf("%.1f %u %d\n", (double)u, (unsigned)(double)u, (int)(-0.9));
  return 0;
}
