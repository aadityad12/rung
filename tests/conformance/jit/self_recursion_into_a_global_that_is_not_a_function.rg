// The global a function calls itself through can stop being a function. The self-call's check
// fails and the VM makes the call, raising its own error on the line of the call.
fn down(n) {
  if (n == 0) return 0;
  return down(n - 1); // expect runtime error: can only call functions
}
print down(3); // expect: 0
let saved = down;
down = 7;
saved(3);
