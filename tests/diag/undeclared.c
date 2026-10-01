// expect: 5:10: error: use of undeclared identifier 'cnt'
int main(void) {
  int count = 0;
  count++;
  return cnt + 1;
}
