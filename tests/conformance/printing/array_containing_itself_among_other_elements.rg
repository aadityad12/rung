let a = [1, 0, 2];
a[1] = a;
print a; // expect: [1, [...], 2]
