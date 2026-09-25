fn fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
print fib(0); // expect: 0
print fib(1); // expect: 1
print fib(10); // expect: 55
print fib(20); // expect: 6765
