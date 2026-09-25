fn f() {
  let n = 0;
  for (;;) {
    n = n + 1;
    if (n == 5) return n;
  }
}
print f(); // expect: 5
