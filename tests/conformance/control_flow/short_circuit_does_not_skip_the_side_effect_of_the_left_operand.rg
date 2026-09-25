let n = 0;
fn bump() { n = n + 1; return false; }
print bump() and bump(); // expect: false
print bump() or bump(); // expect: false
print n; // expect: 3
