// The smallest int, -2147483648, as an index in compiled code. Read as an unsigned 32-bit
// number it would be 2147483648; sign-extended it is negative, and out of range either way.
fn put(a, i, v) {
  a[i] = v; // expect runtime error: array index out of range
  return a;
}
let a = [1, 2];
print put(a, 1, nil); // expect: [1, nil]
print put(a, -2147483648, 0);
