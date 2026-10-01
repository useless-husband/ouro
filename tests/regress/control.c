// Loops, switch, goto, short-circuit evaluation and conditionals.
#include <stdio.h>

static int calls;
static int side(int v) { calls++; return v; }

static const char *classify(int n) {
  switch (n) {
  case 0: return "zero";
  case 1: case 2: case 3: return "small";
  case 10: return "ten";
  case -1: return "minus one";
  case 1000000: return "million";
  default: return "other";
  }
}

static int dense(int n) {
  switch (n) {
  case 0: return 10;
  case 1: return 11;
  case 2: return 12;
  case 3: return 13;
  case 4: return 14;
  case 5: return 15;
  case 7: return 17;
  default: return -1;
  }
}

static int fallthrough(int n) {
  int r = 0;
  switch (n) {
  case 1: r += 1;
  case 2: r += 2;
  case 3: r += 3; break;
  case 4: r += 4;
  default: r += 100;
  }
  return r;
}

int main(void) {
  for (int i = -2; i < 12; i++)
    printf("%d:%s:%d ", i, classify(i), dense(i));
  printf("\n%s\n", classify(1000000));
  for (int i = 0; i < 6; i++)
    printf("%d ", fallthrough(i));
  printf("\n");

  int n = 0;
  while (n < 10) {
    n++;
    if (n == 3) continue;
    if (n == 8) break;
    printf("%d", n);
  }
  printf("\n");
  int k = 0;
  do { k += 3; } while (k < 20);
  printf("%d\n", k);

  int r = side(0) && side(1);
  int s = side(1) || side(0);
  int t = side(1) && side(2) && side(0);
  printf("%d %d %d calls=%d\n", r, s, t, calls);
  printf("%d %d\n", n > 5 ? n * 2 : -n, !n);

  int i = 0;
again:
  i++;
  if (i < 5) goto again;
  printf("goto %d\n", i);

  int sum = 0;
  for (int a = 0; a < 5; a++)
    for (int b = 0; b < 5; b++) {
      if (b > a) break;
      sum += a * b;
    }
  printf("nested %d\n", sum);
  return 0;
}
