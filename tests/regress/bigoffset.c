// Member offsets beyond the range of add/sub immediates (20 MB into a
// struct): the constant is built in a scratch register that must not
// be the reloaded base.
#include <stdio.h>
#include <stdlib.h>
struct Huge { char pad[20000000]; long tail[4]; };
static long g(struct Huge *h, long k) { h->tail[1] = k; return h->tail[1] + h->tail[0]; }
int main(void) {
  struct Huge *h = calloc(1, sizeof *h);
  h->tail[0] = 5;
  printf("%ld\n", g(h, 37));
  free(h);
  return 0;
}
