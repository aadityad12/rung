fn f() {
  print 0.0; // expect: 0.0
  print -0.0; // expect: -0.0
  print 0.0; // expect: 0.0
  print 1 / (0.0 * -1); // expect: -inf
  print 1 / 0.0; // expect: inf
}
f();
