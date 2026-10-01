// expect: 5:10: error: indirection requires pointer operand ('int' invalid)
#define DEREF(p) (*(p))
int main(void) {
  int n = 3;
  return DEREF(n);
}
