let a = "global";
fn f() {
  fn g() { return a; }
  return g;
  let a = "local";
}
print f()(); // expect: global
