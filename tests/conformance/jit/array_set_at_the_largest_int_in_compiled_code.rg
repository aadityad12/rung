// The largest int as an index in a compiled INDEX_SET.
fn put(a, i, v) {
  a[i] = v; // expect runtime error: array index out of range
  return a;
}
let a = [1, 2];
print put(a, 0, 1.5); // expect: [1.5, 2]
print put(a, 2147483647, 0);
