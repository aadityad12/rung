// ARM64 SDIV returns 0 for a zero divisor. The machine code checks the divisor and bails out,
// and the VM raises the error with its own message and the line of the `/`.
fn ratio(a, b) {
  let q = a
    / b; // expect runtime error: division by zero
  return q;
}
print ratio(7, 2); // expect: 3
print ratio(-7, 2); // expect: -3
print ratio(1, 0);
print "not reached";
