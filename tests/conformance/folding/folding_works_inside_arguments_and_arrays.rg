fn add(a, b) {
  return a + b;
}
print add(1 + 1, 2 * 3); // expect: 8
let a = [1 + 1, 2 * 3, 10 / 4];
print a; // expect: [2, 6, 2]
a[1 + 0] = 4 + 4;
print a[0 + 1]; // expect: 8
print len([1, 2, 3]) + 1 + 2; // expect: 6
