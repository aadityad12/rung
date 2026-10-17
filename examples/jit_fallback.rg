// --engine=jit --jit-threshold=1 --jit-log compiles add on its first call.
// The float in the second call fails the integer guard; the VM finishes that call.
fn add(a, b) {
  return a + b;
}
print add(20, 22);
print add(20, 2.5);
