// An empty array has no valid index: its begin and end pointers are equal, so its length is 0.
fn get(a, i) {
  return a[i]; // expect runtime error: array index out of range
}
print get([5], 0); // expect: 5
print get([], 0);
