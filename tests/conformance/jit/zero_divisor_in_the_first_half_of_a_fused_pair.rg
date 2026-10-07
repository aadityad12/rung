// With --superinstructions `i % d + 1` is one fused MOD + ADD word. A zero divisor in its first
// half leaves the machine code at that word, and the VM reports the error on the `%`'s line.
fn f(d, n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    s = i % d // expect runtime error: division by zero
      + 1;
    i = i + 1;
  }
  return s;
}
print f(3, 5); // expect: 2
print f(0, 5);
