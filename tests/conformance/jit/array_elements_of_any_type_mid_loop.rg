// A compiled INDEX_GET reads any value without a guard (the element's type is not its
// business); the ADD after it guards the int path. The float element fails that guard in the
// middle of the loop, and the VM finishes the loop with the frame the machine code left.
fn total(a, n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    s = s + a[i];
    i = i + 1;
  }
  return s;
}
print total([1, 2, 3, 4], 4); // expect: 10
print total([1, 2, 2.5, 4], 4); // expect: 9.5
print total([1, 2, 3, 4], 4); // expect: 10
print total([nil, true, "s"], 0); // expect: 0
