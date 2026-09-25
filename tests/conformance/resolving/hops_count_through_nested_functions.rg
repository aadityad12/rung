fn outer() {
  let a = "a";
  fn middle() {
    let b = "b";
    fn inner() {
      return a + b;
    }
    return inner;
  }
  return middle;
}
print outer()()(); // expect: ab
