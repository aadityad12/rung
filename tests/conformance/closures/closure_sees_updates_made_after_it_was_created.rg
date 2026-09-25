fn f() {
  let x = "before";
  fn show() { return x; }
  x = "after";
  return show;
}
print f()(); // expect: after
