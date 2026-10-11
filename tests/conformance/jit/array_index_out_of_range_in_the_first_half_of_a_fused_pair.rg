// With --superinstructions, `a[j] = false;` and `j = j + step;` are one fused INDEX_SET + ADD
// word. When j runs past the end, the bounds check in the first half leaves the machine code at
// that word; the VM re-runs the pair and reports the line of the `[`, not the `+`.
fn fill(a, n, step) {
  let j = 0;
  while (j <= n) {
    a[j] = false; // expect runtime error: array index out of range
    j = j + step;
  }
  return j;
}
let a = array(10, true);
print fill(a, 9, 3); // expect: 12
print a; // expect: [false, true, true, false, true, true, false, true, true, false]
print fill(a, 10, 5);
