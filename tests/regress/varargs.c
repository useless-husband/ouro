// Variadic functions on both sides of the call (Apple passes every
// variadic argument on the stack).
#include <stdio.h>
#include <stdarg.h>

struct S3 { int a, b, c; };
struct L { long a, b, c; };

static long sum_ints(int n, ...) {
  va_list ap;
  va_start(ap, n);
  long s = 0;
  for (int i = 0; i < n; i++) s += va_arg(ap, int);
  va_end(ap);
  return s;
}

static double mixed(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  double total = 0;
  for (const char *p = fmt; *p; p++) {
    if (*p == 'i') total += va_arg(ap, int);
    else if (*p == 'l') total += va_arg(ap, long);
    else if (*p == 'd') total += va_arg(ap, double);
    else if (*p == 's') { struct S3 s = va_arg(ap, struct S3); total += s.a + s.b + s.c; }
    else if (*p == 'L') { struct L l = va_arg(ap, struct L); total += l.a * l.b * l.c; }
  }
  va_end(ap);
  return total;
}

static void my_printf(const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  vprintf(fmt, ap);
  vprintf(fmt, ap2);
  va_end(ap2);
  va_end(ap);
}

static int many(int a, int b, int c, int d, int e, int f, int g, int h, int i, char j, short k, ...) {
  va_list ap;
  va_start(ap, k);
  int x = va_arg(ap, int);
  double y = va_arg(ap, double);
  va_end(ap);
  return a + b + c + d + e + f + g + h + i + j + k + x + (int)y;
}

int main(void) {
  printf("%ld\n", sum_ints(5, 1, 2, 3, 4, 5));
  struct S3 s = {1, 2, 3};
  struct L l = {2, 3, 4};
  printf("%g\n", mixed("ildsL", 10, 20L, 0.5, s, l));
  my_printf("%s-%d-%c|", "x", 7, 'z');
  printf("\n");
  printf("%d\n", many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13.75));
  printf("%5.2f|%-6d|%06x|%s\n", 3.14159, 42, 255, "end");
  return 0;
}
