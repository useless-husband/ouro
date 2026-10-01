// SCCP sees that the loop never changes `k`, even though it is assigned
// inside the loop (a phi of equal constants on the executable edges).
// RUN: -O1
int g(int n) {
  int k = 7;
  for (int i = 0; i < n; i++)
    if (k != 7)
      k = i;
  return k * 2;
}
// CHECK-LABEL: function _g
// CHECK: ret 14
