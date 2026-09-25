fn double(x) { return x * 2; }
let f = double;
print f(4); // expect: 8
print f == double; // expect: true
