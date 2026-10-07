// Only a `let` defines a name: however often `set` assigns `x`, `y` stays undefined.
let x = 0;
fn read() { return x; }
fn set() { x = x + 1; }
print read(); // expect: 0
set();
set();
print read(); // expect: 2
fn read_y() { return y; } // expect runtime error: undefined variable 'y'
read_y();
