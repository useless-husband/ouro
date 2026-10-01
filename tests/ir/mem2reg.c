// Scalar locals become SSA values: no stack slots, a phi for the loop.
// RUN: -O1
int sum_to(int n) {
  int s = 0;
  for (int i = 1; i <= n; i++)
    s += i;
  return s;
}
// CHECK-LABEL: function _sum_to
// CHECK-NOT: alloca
// CHECK-NOT: load
// CHECK: phi i32
// CHECK: ret
