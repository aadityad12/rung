// Run it with `rung examples/fib.rg`. It prints 6765.
fn fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
print fib(20);
