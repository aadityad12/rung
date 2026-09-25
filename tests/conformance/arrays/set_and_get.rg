let a = array(3, 0);
a[0] = 1;
a[2] = 3;
print a; // expect: [1, 0, 3]
a[0] = a[0] + a[2];
print a[0]; // expect: 4
