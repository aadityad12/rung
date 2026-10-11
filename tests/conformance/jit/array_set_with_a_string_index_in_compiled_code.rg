// A string index in a compiled INDEX_SET: the array guard passes, the int guard fails.
fn put(a, i, v) {
  a[i] = v; // expect runtime error: array index must be an int
  return a;
}
let a = [1, 2, 3];
print put(a, 0, 9); // expect: [9, 2, 3]
print put(a, "0", 9);
