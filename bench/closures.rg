// Upvalue capture and allocation: each iteration creates a closure that captures a local, calls
// it, and drops it. A second closure shares a captured counter across calls.
fn make_adder(n) {
  fn add(x) {
    return x + n;
  }
  return add;
}

fn make_counter() {
  let count = 0;
  fn next() {
    count = count + 1;
    return count;
  }
  return next;
}

fn run() {
  let sum = 0;
  let i = 0;
  while (i < 300000) {
    let add = make_adder(i);
    sum = (sum + add(1)) % 1000003;
    i = i + 1;
  }
  let next = make_counter();
  let j = 0;
  while (j < 300000) {
    sum = (sum + next()) % 1000003;
    j = j + 1;
  }
  return sum;
}
