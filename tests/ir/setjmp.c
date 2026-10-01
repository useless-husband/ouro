// A function that calls setjmp keeps its variables in memory, so their
// values survive a longjmp (the callee-saved registers would not).
// RUN: -O1
typedef int jmp_buf[48];
int setjmp(jmp_buf);
int counter(jmp_buf env) {
  int count = 0;
  if (setjmp(env) == 0)
    count++;
  return count;
}
// CHECK-LABEL: function _counter
// CHECK: alloca 4
