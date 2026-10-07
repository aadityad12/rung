fn count() {
  let n = 0;
  while (true) {
    n = n + 1;
    if (n == 3) return n;
  }
}
print count(); // expect: 3
