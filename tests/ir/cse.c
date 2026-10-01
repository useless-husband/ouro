// The common subexpression a * b is computed once.
// RUN: -O1
long h(long a, long b) {
  long x = a * b + 1;
  long y = a * b - 1;
  return x * y;
}
// CHECK-LABEL: function _h
// CHECK-COUNT-2: mul i64
