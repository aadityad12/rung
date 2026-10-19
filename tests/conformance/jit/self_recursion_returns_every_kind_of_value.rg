// After a self-call returns, the caller copies the result out of the callee's registers. Each
// kind of return is handled separately: a register, a constant, `nil`, and falling off the end.
fn kinds(n) {
  if (n == 0) return nil;
  if (n == 1) return true;
  if (n == 2) return 7;
  if (n == 3) {
    let r = n * 10;
    return r;
  }
  return kinds(n - 4);
}
print kinds(4); // expect: nil
print kinds(5); // expect: true
print kinds(6); // expect: 7
print kinds(7); // expect: 30
fn nothing(n) {
  if (n > 0) nothing(n - 1);
}
print nothing(3); // expect: nil
