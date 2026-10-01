// Results that nothing uses are removed.
// RUN: -O1
int m(int a, int b) {
  int unused = a * b + 42;
  int also = unused << 3;
  (void)also;
  return a - b;
}
// CHECK-LABEL: function _m
// CHECK-NOT: mul
// CHECK-NOT: shl
// CHECK: sub i32
