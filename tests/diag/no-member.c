// expect: 4:33: error: no member named 'z' in 'struct P'
struct P { int x, y; };
int main(void) {
  struct P p = {1, 2}; return p.z;
}
