// Both functions are created before `late` exists. Neither is called until it does.
fn get() { return late; }
fn set(v) { late = v; }
let late = "first";
print get(); // expect: first
set("second");
print get(); // expect: second
set("third");
print late; // expect: third
