// Binary trees (allocation and recursion; from the Computer Language
// Benchmarks Game, single-threaded).
#include <stdio.h>
#include <stdlib.h>

typedef struct node { struct node *left, *right; } node;

static node *make(int depth) {
  node *n = malloc(sizeof(node));
  if (depth > 0) {
    n->left = make(depth - 1);
    n->right = make(depth - 1);
  } else {
    n->left = n->right = NULL;
  }
  return n;
}

static int check(node *n) {
  return n->left ? 1 + check(n->left) + check(n->right) : 1;
}

static void destroy(node *n) {
  if (n->left) {
    destroy(n->left);
    destroy(n->right);
  }
  free(n);
}

int main(int argc, char **argv) {
  int max_depth = argc > 1 ? atoi(argv[1]) : 18;
  int stretch = max_depth + 1;
  node *t = make(stretch);
  printf("stretch tree of depth %d\t check: %d\n", stretch, check(t));
  destroy(t);
  node *long_lived = make(max_depth);
  for (int d = 4; d <= max_depth; d += 2) {
    int iters = 1 << (max_depth - d + 4);
    int sum = 0;
    for (int i = 0; i < iters; i++) {
      node *x = make(d);
      sum += check(x);
      destroy(x);
    }
    printf("%d\t trees of depth %d\t check: %d\n", iters, d, sum);
  }
  printf("long lived tree of depth %d\t check: %d\n", max_depth, check(long_lived));
  return 0;
}
