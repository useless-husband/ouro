// expect: 4:12: error: expected ';' after expression
int main(void) {
  int x = 1;
  x = x + 2
  return x;
}
