// Bit-field layout against clang's (AAPCS64 as used by Apple): fields
// packed within naturally aligned units of their declared type,
// zero-width fields rounding up, unnamed fields not raising alignment.
#include <stdio.h>
#include <string.h>

struct A { char a; int : 0; char b; };
struct B { char a; long : 0; char b; };
struct C { char a; int b : 4; char c; };
struct D { short a : 3; char b; int c : 20; };
struct E { char a; int : 3; };
struct F { int a : 31; int b : 2; };
struct G { long a : 40; long b : 30; char c; };
struct H { unsigned char a : 1, b : 7, c : 8; };
struct I { char a; short b : 9; char c; };
union J { int a : 3; char b; long c : 33; };

#define SHOW(T, init)                                                                      \
  do {                                                                                     \
    T v;                                                                                   \
    memset(&v, 0, sizeof v);                                                               \
    init;                                                                                  \
    unsigned char *p = (unsigned char *)&v;                                                \
    printf("%-9s size %2zu align %zu bytes", #T, sizeof(T), _Alignof(T));                  \
    for (size_t i = 0; i < sizeof(T); i++)                                                 \
      printf(" %02x", p[i]);                                                               \
    printf("\n");                                                                          \
  } while (0)

int main(void) {
  SHOW(struct A, (v.a = 1, v.b = 2));
  SHOW(struct B, (v.a = 1, v.b = 2));
  SHOW(struct C, (v.a = 1, v.b = -3, v.c = 5));
  SHOW(struct D, (v.a = 3, v.b = 7, v.c = 0xABCDE));
  SHOW(struct E, (v.a = 9));
  SHOW(struct F, (v.a = 0x12345678, v.b = 1));
  SHOW(struct G, (v.a = 0x123456789AL, v.b = 0x2ABCDEF0, v.c = 7));
  SHOW(struct H, (v.a = 1, v.b = 0x55, v.c = 0xEE));
  SHOW(struct I, (v.a = 1, v.b = 0x1FF, v.c = 3));
  SHOW(union J, (v.c = 0x1FFFFFFFFL));
  return 0;
}
