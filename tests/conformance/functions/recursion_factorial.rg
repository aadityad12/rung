fn fact(n) {
  if (n <= 1) return 1;
  return n * fact(n - 1);
}
print fact(1); // expect: 1
print fact(5); // expect: 120
print fact(10); // expect: 3628800
