// The addition is the second half of a fused pair; its error is reported on the addition's line,
// not on the remainder's.
fn f(x, y) {
  let m = x % 3;
  let n = m + // expect runtime error: operands must be two numbers or two strings
          y;
  return n;
}
print f(7, 5); // expect: 6
print f(7, nil);
