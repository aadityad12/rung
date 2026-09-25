let a = [1, 2];
let b = a;
b[0] = 99;
print a; // expect: [99, 2]
print a == b; // expect: true
