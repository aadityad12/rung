// A division followed directly by an addition: the Newton step `(g + x / g) / 2.0` has one.
fn root(x) {
  let g = x;
  if (x > 1.0) g = x / 2.0;
  let k = 0;
  while (k < 12) {
    g = (g + x / g) / 2.0;
    k = k + 1;
  }
  return g;
}
print root(16.0); // expect: 4.0
print root(2.25); // expect: 1.5

let q = 7 / 2; let r = q + 1;
print r;          // expect: 4
let f = 7.0 / 2; let s = f + 1;
print s;          // expect: 4.5
let inf = 1.0 / 0.0; let t = inf + 1;
print t;          // expect: inf
let u = -2147483647 / -1; let v = u + 1;
print v;          // expect: -2147483648
