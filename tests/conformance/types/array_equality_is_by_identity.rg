let a = [1];
let b = a;
print a == b; // expect: true
print a == [1]; // expect: false
print [1] == [1]; // expect: false
print [] == []; // expect: false
print a != [1]; // expect: true
