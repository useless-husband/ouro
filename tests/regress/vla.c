// Variable length arrays: run-time sizeof, element access, and stack
// space released at the end of each iteration (200000 iterations of a
// 4 KiB array would overflow the stack otherwise), also on continue and
// break.
#include <stdio.h>
#include <string.h>
static long sum(int n) {
  long a[n];
  for (int i = 0; i < n; i++) a[i] = i * i;
  long s = 0;
  for (int i = 0; i < n; i++) s += a[i];
  return s + (long)sizeof a;
}
static int loop(int iters, int n) {
  int total = 0;
  for (int k = 0; k < iters; k++) {
    char buf[n];
    memset(buf, k & 0x7f, sizeof buf);
    if (k % 3 == 0) continue;
    total += buf[n - 1];
    if (k > iters - 2) break;
  }
  return total;
}
int main(int argc, char **argv) {
  printf("%ld %ld\n", sum(10), sum(1000));
  printf("%d\n", loop(200000, 4096)); // would exhaust the stack without release
  int m = argc + 4;
  double d[m];
  d[0] = 1.5; d[m - 1] = 2.5;
  printf("%zu %g\n", sizeof d / sizeof d[0], d[0] + d[m - 1]);
  return 0;
}
