fn make_adder(n) {
  fn add(x) { return x + n; }
  return add;
}
let add5 = make_adder(5);
let add10 = make_adder(10);
print add5(1); // expect: 6
print add10(1); // expect: 11
print add5(add10(0)); // expect: 15
