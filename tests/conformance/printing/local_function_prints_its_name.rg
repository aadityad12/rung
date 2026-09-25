fn outer() {
  fn inner() { }
  return inner;
}
print outer(); // expect: <fn inner>
