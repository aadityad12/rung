fn down(n) {
  if (n == 0) return 0;
  return 1 + down(n - 1);
}
print down(6000); // expect: 6000
print down(6000); // expect: 6000
print down(6000); // expect: 6000
