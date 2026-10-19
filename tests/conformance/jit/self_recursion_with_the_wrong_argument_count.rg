// A self-call with the wrong number of arguments is left to the VM, which raises the arity
// error on the line of the call.
fn f(n) {
  if (n == 0) return 0;
  return f(n - 1, 0); // expect runtime error: expected 1 arguments but got 2
}
print f(0); // expect: 0
f(2);
