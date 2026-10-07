// Pure dispatch: a tight `while` loop of int arithmetic with no calls and no allocation.
// The sum is kept in range with `%`, so no value wraps (notes D1) and the checksum is easy to
// check by hand.
fn run() {
  let sum = 0;
  let i = 0;
  while (i < 2000000) {
    sum = (sum + i % 7) % 1000003;
    i = i + 1;
  }
  return sum;
}
