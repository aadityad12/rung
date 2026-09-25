fn make() {
  let tag = "kept";
  fn middle() {
    fn inner() { return tag; }
    return inner;
  }
  return middle();
}
print make()(); // expect: kept
