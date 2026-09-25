fn f() { }
let a = [1, 2.5, "s", true, nil, f, [1]];
print a; // expect: [1, 2.5, s, true, nil, <fn f>, [1]]
