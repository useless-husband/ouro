// A variable whose address escapes stays in memory; a volatile one is
// not promoted either; the others are.
// RUN: -O1
void use(int *);
int n(int a) {
  int kept = a;
  volatile int v = a;
  int promoted = a + 1;
  use(&kept);
  return kept + v + promoted;
}
// CHECK-LABEL: function _n
// CHECK-COUNT-2: alloca 4
// CHECK: load volatile
