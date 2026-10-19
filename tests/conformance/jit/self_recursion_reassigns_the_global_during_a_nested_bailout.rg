// The callback forces a nested CALL bailout. It allocates while all recursive VM frames
// are live, then changes the global. Each suspended caller must use the replacement for
// its second call, rather than re-entering stale machine code for the original function.
fn recur(n, callbacks) {
  if (n == 0) return callbacks[0]();
  return recur(n - 1, callbacks) + recur(0, callbacks);
}
fn replacement(n, callbacks) { return 10; }
fn change() {
  let garbage = array(100, nil);
  recur = replacement;
  return 1;
}
let callbacks = [change];
print recur(3, callbacks); // expect: 31
print recur(3, callbacks); // expect: 10
