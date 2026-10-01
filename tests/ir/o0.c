// -O0 does no promotion: every local lives in its stack slot.
// RUN: -O0
int p(int a) {
  int b = a + 1;
  return b;
}
// CHECK-LABEL: function _p
// CHECK: alloca 4
// CHECK: store
// CHECK: load
