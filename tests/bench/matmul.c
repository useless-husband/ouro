// Dense matrix multiplication (nested loops over doubles, 2-D indexing).
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 400;
  double *a = malloc(sizeof(double) * n * n);
  double *b = malloc(sizeof(double) * n * n);
  double *c = malloc(sizeof(double) * n * n);
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++) {
      a[i * n + j] = (double)(i - j) / n;
      b[i * n + j] = (double)(i + j) / n;
      c[i * n + j] = 0;
    }
  for (int i = 0; i < n; i++)
    for (int k = 0; k < n; k++) {
      double aik = a[i * n + k];
      for (int j = 0; j < n; j++)
        c[i * n + j] += aik * b[k * n + j];
    }
  double trace = 0;
  for (int i = 0; i < n; i++)
    trace += c[i * n + i];
  printf("%.6f\n", trace);
  free(a);
  free(b);
  free(c);
  return 0;
}
