// Floating point arithmetic, conversions and comparisons, including NaN.
#include <stdio.h>
#include <math.h>

static double poly(double x) { return ((2.5 * x - 1.25) * x + 0.5) * x - 3; }
static float favg(float a, float b) { return (a + b) / 2; }

int main(void) {
  double d = 3.75;
  float f = -1.5f;
  printf("%g %g %g %g\n", d + f, d * f, d / f, -d);
  printf("%d %d %ld %u\n", (int)d, (int)f, (long)-2.9, (unsigned)3.99);
  printf("%g %g\n", (double)(1L << 53) + 1, (float)16777217);
  printf("%.10g\n", poly(1.1));
  printf("%g\n", favg(1.0f, 2.5f));
  double z = 0.0;
  double nan = z / z;
  printf("%d %d %d %d\n", nan == nan, nan != nan, nan < 1, !(nan >= 1));
  printf("%d %d %d\n", 1.5 < 2, 2.0 <= 2, -0.0 == 0.0);
  unsigned long big = 18446744073709551615UL;
  printf("%.1f %lu\n", (double)big, (unsigned long)1e19);
  printf("%g %g %g\n", sqrt(2.0), floor(-2.5), pow(2, 10));
  float arr[4] = {1.5f, 2.25f, -0.125f, 1e10f};
  float s = 0;
  for (int i = 0; i < 4; i++) s += arr[i];
  printf("%g\n", s);
  long double ld = 1.0L / 3;
  printf("%.15Lg %zu\n", ld, sizeof(long double));
  int i = 7;
  double r = i / 2 + i / 2.0;
  printf("%g\n", r);
  char c = 'A';
  printf("%g %d\n", c * 0.5, (char)(65.9));
  return 0;
}
