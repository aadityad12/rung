// The remainder is the first half of a fused pair; its error is on its own line and the
// addition after it never runs.
fn f(x, zero) {
  let m = x % // expect runtime error: division by zero
          zero;
  let n = m + 1;
  return n;
}
print f(7, 3); // expect: 2
print f(7, 0);
