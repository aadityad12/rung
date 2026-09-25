fn outer() {
  let n = 0;
  fn middle() {
    fn inner() { n = n + 10; }
    inner();
    inner();
  }
  middle();
  return n;
}
print outer(); // expect: 20
