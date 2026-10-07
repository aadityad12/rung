// Conditional jumps test truthiness, which needs no type guard: only nil and false are falsy
// (notes §2.2), so 0 is truthy. `and` / `or` keep the deciding operand's value.
fn either(a, b) { return a or b; }
fn both(a, b) { return a and b; }
fn pick(c) {
  if (c) return 1;
  return 2;
}
print either(nil, 2); // expect: 2
print either(false, 3); // expect: 3
print either(0, 5); // expect: 0
print both(0, 5); // expect: 5
print both(nil, 5); // expect: nil
print both(true, false); // expect: false
print pick(0); // expect: 1
print pick(nil); // expect: 2
print pick(false); // expect: 2
print pick(either); // expect: 1
