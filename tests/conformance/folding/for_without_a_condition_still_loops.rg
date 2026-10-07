fn count() {
  let n = 0;
  for (;;) {
    n = n + 1;
    if (n > 4) return n;
  }
}
print count(); // expect: 5
