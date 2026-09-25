fn down(n) {
  if (n == 0) return 0;
  return 1 + down(n - 1);
}
// down(9998) is 9,999 calls deep.
print down(9998); // expect: 9998
