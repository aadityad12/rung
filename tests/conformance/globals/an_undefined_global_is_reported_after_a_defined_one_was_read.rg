// The function reads `a`, which exists, and then `b`, which does not. The error names `b` and
// the line of the read; the successful read of `a` before it changes nothing.
let a = 1;
fn f() {
  print a; // expect: 1
  return b; // expect runtime error: undefined variable 'b'
}
f();
