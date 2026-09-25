let a = [0, 0];
print a[0] = 5; // expect: 5
print a[1] = a[0] = 7; // expect: 7
print a; // expect: [7, 7]
