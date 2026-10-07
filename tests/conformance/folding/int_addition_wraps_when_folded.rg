// The sum of two constants is computed at compile time with --fold; it must wrap exactly as the
// run-time addition does (notes D1).
print 2147483647 + 1; // expect: -2147483648
print -2147483648 + -1; // expect: 2147483647
print 2147483647 + 2147483647; // expect: -2
