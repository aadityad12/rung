fn make(a, b) {
  fn sum() { return a + b; }
  a = 100;
  return sum;
}
print make(1, 2)(); // expect: 102
