// A zero divisor three self-calls deep: the VM resumes the innermost call at the `/` and raises
// the error with the line of the operator.
fn split(n, d) {
  if (n == 0) return 12
    / d; // expect runtime error: division by zero
  return split(n - 1, d) + 1;
}
print split(3, 4); // expect: 6
split(3, 0);
