// A value live across a call stays in a callee-saved register, which
// the prologue saves; nothing is spilled to the stack.
// RUN: -O1
long g(long);
long f(long a, long b) {
  long x = g(a);
  return x + g(b) + a;
}
// CHECK-LABEL: _f:
// CHECK: stp x19, x20
// CHECK: bl _g
// CHECK-NOT: [sp, #
// CHECK: bl _g
// CHECK: ldp x19, x20
