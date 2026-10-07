// The zero divisor shows up on the fourth trip round a compiled loop; the output of the first
// three trips stays, and the error is the VM's, on the line of the `%`.
fn sum_of_remainders(n, d) {
  let s = 0;
  while (n > 0) {
    s = s + n % d; // expect runtime error: division by zero
    n = n - 1;
    d = d - 1;
  }
  return s;
}
print sum_of_remainders(3, 7); // expect: 6
print sum_of_remainders(10, 3);
