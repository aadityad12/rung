// The array write is the first half of a fused pair and fails on its own line, before the
// addition after it can run.
fn f(a, i) {
  a[i] = // expect runtime error: array index out of range
      1;
  let n = i + 1;
  return n;
}
print f(array(3, 0), 2); // expect: 3
print f(array(3, 0), 3);
