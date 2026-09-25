fn apply(f, x) { return f(x); }
fn inc(n) { return n + 1; }
fn square(n) { return n * n; }
print apply(inc, 4); // expect: 5
print apply(square, 4); // expect: 16
