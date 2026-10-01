// A comparison feeding a branch becomes cmp + b.cond, a test against
// zero becomes cbz/cbnz, and constants use immediate forms.
// RUN: -O1
int f(int a, long *p) {
  if (a > 100)
    return 1;
  if (!p)
    return 2;
  return (a & 0xff00) | 7;
}
// CHECK-LABEL: _f:
// CHECK: cmp w0, #100
// CHECK: b.{{(le|gt)}}
// CHECK: {{cbn?z}} x1
// CHECK: and w{{[0-9]+}}, w0, #0xff00
// CHECK-NOT: cset
