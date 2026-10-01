// Constants propagate through variables and branches; the dead branch
// and its call disappear.
// RUN: -O1
int printf(const char *, ...);
int f(void) {
  int x = 3;
  int y = x * 4;
  if (y > 10)
    return y + 1;
  printf("never\n");
  return 0;
}
// CHECK-LABEL: function _f
// CHECK-NOT: call
// CHECK: ret 13
