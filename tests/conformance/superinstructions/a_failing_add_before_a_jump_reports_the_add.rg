// The addition is the first half of a fused add-and-jump. Its error, on the line of the `+`,
// stops the loop before the jump back.
fn run() {
  let x = 1;
  let n = 0;
  while (n < 3) {
    n = n + 1;
    print n;      // expect: 1
    x = x +       // expect runtime error: operands must be two numbers or two strings
        nil;
  }
}
run();
