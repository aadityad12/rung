// `bump` changes `n` through its upvalue while `n + bump()` is being evaluated. The left
// operand was read before the call, so it keeps the old value.
fn run() {
  let n = 10;
  fn bump() { n = n + 1; return 100; }
  print n + bump(); // expect: 110
  print n; // expect: 11
  print bump() + n; // expect: 112
  let arr = [n, bump(), n];
  print arr; // expect: [12, 100, 13]
}
run();
