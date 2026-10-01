// expect: 3:9: error: variable length arrays are not supported
int f(int n) {
  int a[n];
  return a[0];
}
