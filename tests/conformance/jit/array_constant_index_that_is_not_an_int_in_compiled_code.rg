// A constant index that is not an int can never be valid: that INDEX_SET compiles to a plain
// exit, and the VM raises the error. The function still compiles, and the code before it runs.
fn put(a, flag) {
  a[0] = 1;
  if (flag) a[true] = 2; // expect runtime error: array index must be an int
  return a;
}
print put([0], false); // expect: [1]
print put([0], true);
