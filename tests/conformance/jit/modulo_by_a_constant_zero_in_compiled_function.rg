// A constant zero divisor is known when the function is compiled: the machine code goes straight
// back to the VM, which raises the error on the line of the `%`.
fn wrap(a, wrap_at) {
  if (wrap_at > 0) return a % wrap_at;
  return a % 0; // expect runtime error: division by zero
}
print wrap(17, 5); // expect: 2
print wrap(17, 0);
