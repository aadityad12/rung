fn compose(f, g) {
  fn composed(x) { return f(g(x)); }
  return composed;
}
fn inc(x) { return x + 1; }
fn dbl(x) { return x * 2; }
print compose(inc, dbl)(5); // expect: 11
print compose(dbl, inc)(5); // expect: 12
print compose(compose(inc, inc), dbl)(1); // expect: 4
