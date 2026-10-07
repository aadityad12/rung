// A string reaches a compiled `-`: the guard fails, the VM runs the SUB itself and raises the
// type error with the line of the operator.
fn diff(a, b) {
  return a
    - b; // expect runtime error: operands must be numbers
}
print diff(5, 3); // expect: 2
print diff("five", 3);
