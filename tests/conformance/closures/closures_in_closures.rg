fn outer(a) {
  fn middle(b) {
    fn inner(c) { return a + b + c; }
    return inner;
  }
  return middle;
}
print outer(1)(2)(3); // expect: 6
