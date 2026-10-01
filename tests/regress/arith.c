// Integer arithmetic, conversions and the usual arithmetic conversions.
#include <stdio.h>
#include <limits.h>
#include <stdint.h>

static int mul_add(int a, int b, int c) { return a * b + c; }

int main(void) {
  int a = 17, b = -5;
  unsigned u = 3000000000u;
  long l = -123456789012L;
  unsigned long ul = 18000000000000000000UL;
  printf("%d %d %d %d %d\n", a + b, a - b, a * b, a / b, a % b);
  printf("%d %d %d\n", -a / 4, -a % 4, a / -4);
  printf("%u %u %u\n", u + 1500000000u, u * 2, u / 7);
  printf("%ld %ld %ld\n", l * 3, l / 1000, l % 1000);
  printf("%lu %lu\n", ul / 3, ul % 1000003);
  printf("%d %d %d\n", a << 3, b >> 1, (int)((unsigned)b >> 28));
  printf("%d %d %d %d\n", a & b, a | b, a ^ b, ~a);
  printf("%d %d\n", -1 < 0u, -1L < 0u);
  printf("%d\n", (unsigned char)300 + (signed char)200);
  printf("%d %d\n", (short)70000, (unsigned short)-1);
  printf("%lld %llu\n", LLONG_MIN, ULLONG_MAX);
  printf("%d %d\n", INT_MAX + 0, INT_MIN / 2);
  int64_t big = (int64_t)1 << 40;
  printf("%lld %lld\n", (long long)big, (long long)(big >> 35));
  printf("%d\n", mul_add(6, 7, -2));
  char c = -1;
  unsigned char uc = 255;
  printf("%d %d %d\n", c, uc, c == uc);
  int x = 10;
  x += 5; x -= 3; x *= 4; x /= 3; x %= 7; x <<= 4; x >>= 2; x &= 0x3c; x |= 0x101; x ^= 0x11;
  printf("%d\n", x);
  int i = 5;
  int j = i++;
  j += ++i;
  printf("%d %d\n", i, j);
  long acc = 0;
  for (int k = 0; k < 100; k++)
    acc += k * k - 3 * k;
  printf("%ld\n", acc);
  unsigned w = 0xFFFFFFFFu;
  w += 1;
  printf("%u %d\n", w, (int)(0x80000000u >> 31));
  return 0;
}
