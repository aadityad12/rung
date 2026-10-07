// The same global read runs many times; each run must see the newest value, not a copy.
let counter = 0;
fn read() { return counter; }
fn bump() { counter = counter + 1; }
let i = 0;
let total = 0;
while (i < 5) {
  bump();
  total = total + read();
  i = i + 1;
}
print total; // expect: 15
print read(); // expect: 5
