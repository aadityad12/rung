// The addition is the second half of a fused pair: the store has been done, and the addition's
// error is on the addition's line, not the store's.
fn f(a, c) {
  a[1] = 7;
  let b = c + // expect runtime error: operands must be two numbers or two strings
          nil;
  return b;
}
print f(array(2, 0), 5);
