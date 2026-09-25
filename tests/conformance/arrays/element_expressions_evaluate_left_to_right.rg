fn say(x) { print x; return x; }
let a = [say(1), say(2), say(3)];
// expect: 1
// expect: 2
// expect: 3
print a; // expect: [1, 2, 3]
