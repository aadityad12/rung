fn outer() {
  fn helper(x) { return x + 1; }
  return helper(helper(1));
}
print outer(); // expect: 3
