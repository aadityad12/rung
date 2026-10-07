// `len` starts as a native function. A call site that has used it must switch to the Rung
// function that later takes the name.
fn count(a) { return len(a); }
print count([1, 2, 3]); // expect: 3
print count("hello"); // expect: 5
fn len(a) { return 99; }
print count([1, 2, 3]); // expect: 99
