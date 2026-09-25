let n = 3;
while (n) {
  print n;
  n = n - 1;
  if (n == 0) n = nil;
}
// expect: 3
// expect: 2
// expect: 1
