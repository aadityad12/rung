// The last valid index is len - 1; the length itself is out of range.
fn get(a, i) {
  return a[i]; // expect runtime error: array index out of range
}
let a = array(4, 7);
print get(a, 3); // expect: 7
print get(a, len(a));
