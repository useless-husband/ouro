// expect: 4:11: error: called object type 'int' is not a function or function pointer
int main(void) {
  int f = 0;
  return f(1);
}
