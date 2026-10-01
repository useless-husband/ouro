// Branches whose condition is a constant or a symbol address, as in
// assert(x && "message").  The instruction selector once emitted the
// branch before materialising such a condition (found by building Lua
// with its internal assertions on).
#include <stdio.h>

int global;
static int calls;
static void fail(const char *msg) { calls++; printf("fail: %s\n", msg); }
#define check(c) ((c) ? (void)0 : fail(#c))

static int f(int x) {
  check(x > 0 && "x must be positive");
  check(x < 100 || "unreachable message");
  if (&global)
    x++;
  if ("literal")
    x += 10;
  while ("loop" && x > 50)
    x -= 7;
  return x;
}

int main(void) {
  printf("%d %d %d\n", f(5), f(-3), f(80));
  printf("calls=%d\n", calls);
  return 0;
}
