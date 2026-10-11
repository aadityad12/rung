// A function is an object of another kind: the kind-byte guard fails in INDEX_SET.
fn put(a, i, v) {
  a[i] = v; // expect runtime error: can only index arrays
  return a;
}
print put([1, 2], 0, true); // expect: [true, 2]
print put(put, 0, 1);
