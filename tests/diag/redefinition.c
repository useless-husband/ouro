// expect: 4:7: error: redefinition of 'x'
int main(void) {
  int x = 1;
  int x = 2;
  return x;
}
