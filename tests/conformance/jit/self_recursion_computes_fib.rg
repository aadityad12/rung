// fib calls itself through the global `fib` (notes D7). With the JIT, every one of those calls
// runs in machine code, which pushes the VM's frames itself (issue #25).
fn fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
print fib(0); // expect: 0
print fib(1); // expect: 1
print fib(10); // expect: 55
print fib(20); // expect: 6765
