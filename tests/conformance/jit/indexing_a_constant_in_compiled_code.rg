// Indexing a literal: `5[0]` names the constant 5 as its array operand, which can never be an
// array, so it compiles to a plain exit and the VM raises the error.
fn f(n) {
  if (n > 0) return n;
  return 5[0]; // expect runtime error: can only index arrays
}
print f(3); // expect: 3
print f(0);
