let a = [0];
let b = [a];
a[0] = b;
print a; // expect: [[[...]]]
print b; // expect: [[[...]]]
