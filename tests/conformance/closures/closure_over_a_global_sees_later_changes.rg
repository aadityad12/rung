let g = 1;
fn read() { return g; }
print read(); // expect: 1
g = 2;
print read(); // expect: 2
