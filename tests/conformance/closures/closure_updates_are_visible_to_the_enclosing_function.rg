fn f() {
  let x = 1;
  fn inc() { x = x + 1; }
  inc();
  inc();
  return x;
}
print f(); // expect: 3
