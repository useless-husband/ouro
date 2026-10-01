// expect: 4:15: error: too few arguments to function call, expected 2, have 1
int add(int a, int b);
int main(void) {
  return add(1);
}
