fn make() {
  fn fact(n) {
    if (n <= 1) return 1;
    return n * fact(n - 1);
  }
  return fact;
}
print make()(5); // expect: 120
