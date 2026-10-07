// A fused pair is two words, and a jump may land on the second one. These programs make the
// compiler jump to the middle of what --superinstructions fuses.
//
// 1. `x = x + 1` is the last statement of an inner `if`, and the outer `if` has an `else`, so the
//    outer `then` branch ends with a jump over the `else`. The addition and that jump are
//    adjacent, and the inner test jumps straight to the jump.
fn one(a, b) {
  let x = 0;
  let y = 0;
  if (a) {
    if (b) x = x + 1;
  } else {
    y = 5;
  }
  return x * 10 + y;
}
print one(true, false);  // expect: 0
print one(true, true);   // expect: 10
print one(false, false); // expect: 5

// 2. A remainder ends the `then` branch and the statement after the `if` is an addition, so the
//    skipped branch lands on the addition: the second half of a remainder-and-addition.
fn two(c) {
  let m = 100;
  let n = 0;
  if (c) m = 7 % 4;
  n = m + 1;
  return n;
}
print two(true);  // expect: 4
print two(false); // expect: 101
