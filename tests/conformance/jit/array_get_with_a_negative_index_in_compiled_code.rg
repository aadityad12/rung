// -1 is sign-extended to 64 bits before the bounds check, so as an unsigned number it is larger
// than any length and the one unsigned compare rejects it.
fn get(a, i) {
  return a[i]; // expect runtime error: array index out of range
}
let a = [1, 2, 3];
print get(a, 0); // expect: 1
print get(a, -1);
