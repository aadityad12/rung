// A constant index is put in a machine register directly, and still bounds-checked: 2 is fine
// for a 3-element array and out of range for a 2-element one.
fn third(a) {
  return a[2]; // expect runtime error: array index out of range
}
print third([1, 2, 3]); // expect: 3
print third([1, 2]);
