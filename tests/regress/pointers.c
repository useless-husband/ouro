// Pointers, arrays, strings, function pointers and recursion.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int cmp_int(const void *a, const void *b) {
  int x = *(const int *)a, y = *(const int *)b;
  return (x > y) - (x < y);
}

static int apply(int (*f)(int, int), int a, int b) { return f(a, b); }
static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static long fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static void reverse(char *s) {
  for (char *e = s + strlen(s) - 1; s < e; s++, e--) { char t = *s; *s = *e; *e = t; }
}

int matrix[3][4];

int main(void) {
  int a[] = {5, 3, 9, 1, 7, 2};
  int n = sizeof a / sizeof a[0];
  qsort(a, n, sizeof(int), cmp_int);
  for (int i = 0; i < n; i++) printf("%d ", a[i]);
  printf("\n");
  int (*ops[2])(int, int) = {add, sub};
  printf("%d %d\n", apply(ops[0], 7, 3), apply(ops[1], 7, 3));
  printf("%ld\n", fib(25));
  char buf[32];
  strcpy(buf, "ouro compiler");
  reverse(buf);
  printf("%s %zu\n", buf, strlen(buf));
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 4; j++)
      matrix[i][j] = i * 10 + j;
  int *flat = &matrix[0][0];
  printf("%d %d %d\n", matrix[2][3], flat[5], *(*(matrix + 1) + 2));
  int *p = a, *q = a + 4;
  printf("%ld %d\n", (long)(q - p), *q);
  char *s = "literal";
  printf("%c %c %s\n", s[0], *(s + 3), s + 4);
  int x = 1, *px = &x, **ppx = &px;
  **ppx = 42;
  printf("%d\n", x);
  void *vp = &x;
  printf("%d\n", *(int *)vp);
  int *heap = malloc(10 * sizeof(int));
  for (int i = 0; i < 10; i++) heap[i] = i * i;
  long total = 0;
  for (int *it = heap; it != heap + 10; ++it) total += *it;
  printf("%ld\n", total);
  free(heap);
  const char *names[] = {"alpha", "beta", "gamma"};
  printf("%s %c\n", names[2], names[1][2]);
  return 0;
}
