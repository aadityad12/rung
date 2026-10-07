// The loop_sum shape (notes D7): the one benchmark the JIT is guaranteed to compile. Under
// --engine=jit the second call runs as machine code at the default threshold too: the loop's
// back-edges make `run` hot during the first call.
fn run() {
  let sum = 0;
  let i = 0;
  while (i < 10000) {
    sum = (sum + i) % 1000007;
    i = i + 1;
  }
  return sum;
}
print run(); // expect: 994657
print run(); // expect: 994657
