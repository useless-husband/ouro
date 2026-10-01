// The loop body is a single register-offset load and two adds; the
// phi copies at the back edge are coalesced away.
// RUN: -O1
int sum(int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += a[i];
  return s;
}
// CHECK-LABEL: _sum:
// CHECK: ldr w{{.*}}, [x0, w{{.*}}, sxtw #2]
// CHECK-NOT: mov x
// CHECK: b.lt
long dot(long *x, long *y, unsigned long n) {
  long s = 0;
  for (unsigned long i = 0; i < n; i++)
    s += x[i] * y[i];
  return s;
}
// CHECK-LABEL: _dot:
// CHECK: lsl #3]
