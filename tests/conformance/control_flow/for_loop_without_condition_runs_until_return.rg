fn count_to(n) {
  for (let i = 0; ; i = i + 1) {
    if (i == n) return i;
  }
}
print count_to(4); // expect: 4
