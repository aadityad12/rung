fn fact(n) {
  if (n <= 1) return 1;
  return n * fact(n - 1);
}
print fact(12); // expect: 479001600
print fact(13); // expect: 1932053504
