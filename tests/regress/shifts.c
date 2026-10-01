// Shifted-register operands next to constants: c + (x << k), 0 - (x << k),
// c - (x << k).  `8 + ((w - 8) << 4)` from zlib's deflate() once made the
// instruction selector drop the shift at -O0.
#include <stdio.h>

static unsigned header(unsigned w) { return (8 + ((w - 8) << 4)) << 8; }
static long neg_shift(long x) { return 0 - (x << 3); }
static long sub_shift(long x) { return 100 - (x << 2); }
static int mixed(int a, int b) { return (a << 2) + (b << 5) - (a << 1); }

int main(void) {
  for (unsigned w = 8; w <= 15; w++)
    printf("%u ", header(w));
  printf("\n%ld %ld %d\n", neg_shift(7), sub_shift(9), mixed(3, -4));
  return 0;
}
