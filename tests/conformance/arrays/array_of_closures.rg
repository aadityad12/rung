fn make(n) {
  fn get() { return n; }
  return get;
}
let fs = [make(1), make(2), make(3)];
print fs[0]() + fs[1]() + fs[2](); // expect: 6
