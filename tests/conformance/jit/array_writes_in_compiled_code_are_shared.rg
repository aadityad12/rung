// Compiled code writes into the array object itself, so the caller, another name for the same
// array, and an array holding it all see the writes (arrays are shared by reference, notes D2).
// Every kind of value can be stored and read back.
fn copy(from, to, n) {
  let i = 0;
  while (i < n) {
    to[i] = from[i];
    i = i + 1;
  }
  return to;
}
fn swap(a, i, j) {
  let t = a[i];
  a[i] = a[j];
  a[j] = t;
}
let a = [1, -2.5, "s", nil, true, copy];
let b = array(6, 0);
let alias = b;
let holder = [b];
copy(a, b, 6);
print alias; // expect: [1, -2.5, s, nil, true, <fn copy>]
swap(b, 0, 5);
print holder[0]; // expect: [<fn copy>, -2.5, s, nil, true, 1]
b[1] = b;
swap(b, 1, 2);
print b; // expect: [<fn copy>, s, [...], nil, true, 1]
