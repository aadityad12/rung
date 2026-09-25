fn f(a) {
  fn g(b) { return [a + b, a * b]; }
  return g;
}
print f(1)(2)[0]; // expect: 3
print f(3)(4)[1]; // expect: 12
let table = [f];
print table[0](5)(6)[0]; // expect: 11
