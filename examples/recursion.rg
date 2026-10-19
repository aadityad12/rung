// The JIT calls its own machine code for both recursive calls.
// With --jit-threshold=1, even the first invocation enters native code.
fn fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
print fib(20);
