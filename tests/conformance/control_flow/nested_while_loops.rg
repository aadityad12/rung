let i = 1;
while (i <= 2) {
  let j = 1;
  while (j <= 3) {
    print i * 10 + j;
    j = j + 1;
  }
  i = i + 1;
}
// expect: 11
// expect: 12
// expect: 13
// expect: 21
// expect: 22
// expect: 23
