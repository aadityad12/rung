let a;
a = 1;
print a; // expect: 1
fn f() { b = 2; } // expect runtime error: undefined variable 'b'
f();
