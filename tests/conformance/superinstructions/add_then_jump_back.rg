// The last statement of a loop body is an addition, and the jump back follows it. Ints wrap.
// (Locals: a global would need a load and a store around the addition.)
fn wrap() {
  let x = 2147483646;
  let n = 0;
  while (n < 3) {
    n = n + 1;
    x = x + 1;
  }
  return x;
}
print wrap(); // expect: -2147483647

fn floats() {
  let total = 0.5;
  let i = 0;
  while (i < 4) {
    i = i + 1;
    total = total + 0.25;
  }
  return total;
}
print floats(); // expect: 1.5

fn strings() {
  let s = "";
  let k = 0;
  while (k < 3) {
    k = k + 1;
    s = s + "ab";
  }
  return s;
}
print strings(); // expect: ababab
