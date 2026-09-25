let i = 0;
while (i < 2) {
  let seen;
  print seen; // expect: nil
  seen = i;
  i = i + 1;
}
// expect: nil
print i; // expect: 2
