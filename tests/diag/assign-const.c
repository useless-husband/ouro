// expect: 4:9: error: cannot assign to variable 'limit' with const-qualified type 'const int'
int main(void) {
  const int limit = 10;
  limit = 11;
  return limit;
}
