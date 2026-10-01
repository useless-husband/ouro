// Multiplication and unsigned division by powers of two become shifts;
// x + 0 and x * 1 vanish.
// RUN: -O1
unsigned k(unsigned x, unsigned y) {
  return (x * 8 + 0) / 16 + y * 1 % 32;
}
// CHECK-LABEL: function _k
// CHECK: shl i32
// CHECK: lshr i32
// CHECK: and i32
// CHECK-NOT: mul
// CHECK-NOT: udiv
// CHECK-NOT: urem
