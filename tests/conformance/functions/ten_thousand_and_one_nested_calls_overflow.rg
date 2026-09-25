fn down(n) {
  if (n == 0) return 0;
  return 1 + down(n - 1); // expect runtime error: stack overflow
}
// down(10000) is 10,001 calls deep.
print down(10000);
