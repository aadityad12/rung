// `get` compiles on its first call (--jit-threshold=1). Called with an int where the array
// should be, the object-tag guard fails, the machine code returns to the VM at INDEX_GET, and
// the VM raises the error on the line of the `[`.
fn get(a, i) {
  return a[i]; // expect runtime error: can only index arrays
}
print get([10, 20, 30], 1); // expect: 20
print get(5, 0);
