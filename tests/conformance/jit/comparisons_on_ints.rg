// All six comparisons through compiled code, including the extremes of the int range.
fn lt(a, b) { return a < b; }
fn le(a, b) { return a <= b; }
fn gt(a, b) { return a > b; }
fn ge(a, b) { return a >= b; }
fn eq(a, b) { return a == b; }
fn ne(a, b) { return a != b; }
let min = -2147483648;
let max = 2147483647;
print lt(min, max); // expect: true
print lt(max, min); // expect: false
print le(3, 3); // expect: true
print le(4, 3); // expect: false
print gt(-1, -2); // expect: true
print gt(-2, -1); // expect: false
print ge(0, 0); // expect: true
print ge(-1, 0); // expect: false
print eq(max, max); // expect: true
print eq(max, min); // expect: false
print ne(1, 2); // expect: true
print ne(2, 2); // expect: false
