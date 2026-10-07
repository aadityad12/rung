// Every int rule of notes §2.1 on values the machine code only sees at run time (parameters,
// not constants), so 32-bit W-register arithmetic does the wrapping.
fn add(a, b) { return a + b; }
fn sub(a, b) { return a - b; }
fn mul(a, b) { return a * b; }
fn div(a, b) { return a / b; }
fn mod(a, b) { return a % b; }
fn neg(a) { return -a; }
let max = 2147483647;
let min = -2147483648;
print add(max, 1); // expect: -2147483648
print sub(min, 1); // expect: 2147483647
print mul(65536, 65536); // expect: 0
print mul(max, 2); // expect: -2
print mul(min, -1); // expect: -2147483648
print div(min, -1); // expect: -2147483648
print div(-7, 2); // expect: -3
print mod(min, -1); // expect: 0
print mod(-7, 3); // expect: -1
print mod(7, -3); // expect: 1
print neg(min); // expect: -2147483648
print neg(5); // expect: -5
