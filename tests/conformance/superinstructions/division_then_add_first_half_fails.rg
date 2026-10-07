// Division by zero in the first half of a fused division-and-addition. The output printed before
// the error is kept, and the addition that follows never runs.
fn f(x, d) {
  let q = x / // expect runtime error: division by zero
            d;
  let r = q + 1;
  return r;
}
print f(8, 2); // expect: 5
print f(8, 0);
