fn first_multiple_of_seven_above(n) {
  while (true) {
    n = n + 1;
    if (n % 7 == 0) return n;
  }
}
print first_multiple_of_seven_above(20); // expect: 21
print first_multiple_of_seven_above(21); // expect: 28
