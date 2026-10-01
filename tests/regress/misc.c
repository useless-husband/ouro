// Assorted constructs: K&R definitions (float parameters arrive as
// double), flexible array members, member access on call results,
// struct-valued conditionals, bit-fields of every flavour, declarators
// for functions returning function pointers, negative enumerators,
// pointer differences with odd element sizes, and literal forms.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <wchar.h>

double knr(a, b, c)
  float a;
  char b;
  double c;
{
  return a + b + c;
}

struct Vec { int n; double v[]; };

struct Pt { int x, y; };
static struct Pt pt(int x, int y) { struct Pt p = {x, y}; return p; }

struct Flags { bool on : 1; unsigned mode : 3; int delta : 5; unsigned long big : 33; };
union U { struct { unsigned lo : 4, hi : 4; } nib; unsigned char byte; };

static int add1(int x) { return x + 1; }
static int dbl(int x) { return x * 2; }
static int (*pick(int which))(int) { return which ? dbl : add1; }
typedef int (*op_fn)(int);
static op_fn table[2] = {add1, dbl};

enum Temp { COLD = -10, MILD = 15, HOT = 40 };

struct Odd { char c[3]; };

int main(void) {
  double (*k)() = knr;
  printf("%g\n", k(1.5f, 'A', 0.25));

  struct Vec *v = malloc(sizeof(struct Vec) + 4 * sizeof(double));
  v->n = 4;
  for (int i = 0; i < v->n; i++) v->v[i] = i * 1.5;
  printf("%zu %g\n", sizeof(struct Vec), v->v[3]);
  free(v);

  printf("%d %d\n", pt(3, 4).y, pt(5, 6).x + pt(7, 8).y);
  int flag = 1;
  struct Pt a = pt(1, 2), b = pt(9, 9);
  struct Pt c = flag ? a : b;
  printf("%d %d\n", c.x, (flag ? b : a).y);

  struct Flags f = {true, 5, -3, 0x1FFFFFFFFUL};
  f.mode += 4;
  f.delta = f.delta * 5;
  f.on = !f.on;
  printf("%d %u %d %lx %zu\n", f.on, f.mode, f.delta, (unsigned long)f.big, sizeof f);
  union U u;
  u.byte = 0xA7;
  printf("%u %u\n", u.nib.lo, u.nib.hi);
  u.nib.hi = 0x3;
  printf("%x\n", u.byte);

  printf("%d %d %d\n", pick(0)(10), pick(1)(10), table[1](table[0](4)));
  enum Temp t = COLD;
  printf("%d %d %d\n", t < MILD, t + HOT, (int)sizeof(enum Temp));
  struct Odd arr[10];
  struct Odd *p = &arr[7], *q = &arr[2];
  printf("%ld %ld\n", (long)(p - q), (long)(q - p));
  unsigned char uc = 250;
  int laps = 0;
  for (unsigned char i = uc; i != 4; i++) laps++;
  printf("%d\n", laps);
  printf("%d %d %d\n", 'ab', '\377', (int)L'\x263A');
  wchar_t w[] = L"wide" L"r";
  printf("%zu %d\n", sizeof w / sizeof w[0], w[4]);
  const char *cat = "con" "cat" "enated";
  printf("%s %zu\n", cat, strlen(cat));
  printf("%lld %llu\n", -9223372036854775807LL - 1, 0xFFFFFFFFFFFFFFFFULL);
  return 0;
}
