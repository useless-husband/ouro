// expect: 5:16: error: assigning 'struct P' to incompatible type 'int'
struct P { int x, y; };
int main(void) {
  struct P p = {1, 2};
  int i = 0; i = p;
  return i;
}
