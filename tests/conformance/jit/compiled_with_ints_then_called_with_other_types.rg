// Compiled while every argument is an int; later calls pass floats and strings, which fail the
// type guards, so the VM's own rules give the answers (int + float is a float, string + string
// concatenates, and 1 == 1.0).
fn add(a, b) { return a + b; }
fn same(a, b) { return a == b; }
fn less(a, b) { return a < b; }
print add(1, 2); // expect: 3
print same(1, 1); // expect: true
print less(1, 2); // expect: true
print add(1, 2.5); // expect: 3.5
print add(0.25, 1); // expect: 1.25
print add("ab", "cd"); // expect: abcd
print same(1, 1.0); // expect: true
print same("x", "x"); // expect: true
print same(nil, false); // expect: false
print less(1.5, 1); // expect: false
print add(40, 2); // expect: 42
