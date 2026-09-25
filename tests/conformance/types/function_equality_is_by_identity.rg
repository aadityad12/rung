fn f() { }
fn g() { }
print f == f; // expect: true
print f == g; // expect: false
let h = f;
print h == f; // expect: true
print f != g; // expect: true
