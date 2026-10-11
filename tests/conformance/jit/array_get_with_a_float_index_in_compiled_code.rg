// A float index fails the int guard on the index, even one with an integral value.
fn get(a, i) {
  return a[i]; // expect runtime error: array index must be an int
}
let a = [1, 2, 3];
print get(a, 2); // expect: 3
print get(a, 1.0);
