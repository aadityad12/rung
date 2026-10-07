// `let` on an existing global replaces its value; a function that already read it must see the
// new one on its next call.
let x = 1;
fn show() { print x; }
show(); // expect: 1
let x = 2;
show(); // expect: 2
let x = "three";
show(); // expect: three
