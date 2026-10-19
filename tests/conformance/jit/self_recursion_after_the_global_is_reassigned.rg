// `countdown` reads the global `countdown` to call itself. Once the global names another
// function, a call still running the old one must call the new one: the machine code checks
// which function the global holds before every self-call, and leaves the call to the VM if it is
// not this one.
fn countdown(n) {
  if (n == 0) return 0;
  return countdown(n - 1) + 1;
}
fn hundred(n) { return 100; }
print countdown(5); // expect: 5
let old = countdown;
countdown = hundred;
print old(5); // expect: 101
print countdown(5); // expect: 100
