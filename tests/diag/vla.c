// expect: 3:8: error: multi-dimensional variable length arrays are not supported
int f(int n) {
  int a[n][n];
  return a[0][0];
}
