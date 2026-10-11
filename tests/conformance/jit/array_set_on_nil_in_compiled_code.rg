// INDEX_SET in compiled code with nil where the array should be.
fn put(a, i, v) {
  a[i] = v; // expect runtime error: can only index arrays
  return a;
}
print put([1, 2], 1, 5); // expect: [1, 5]
print put(nil, 0, 1);
