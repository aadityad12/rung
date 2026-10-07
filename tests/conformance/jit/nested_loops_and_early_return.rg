// Loops inside loops, a for loop, a loop left by `return`, and wraparound piling up in a hash:
// all inside the whitelist, so all of it runs as machine code under --engine=jit.
fn table_sum(n) {
  let total = 0;
  for (let i = 1; i <= n; i = i + 1) {
    let j = 1;
    while (j <= n) {
      total = total + i * j;
      j = j + 1;
    }
  }
  return total;
}
fn first_square_above(limit) {
  let k = 0;
  while (true) {
    if (k * k > limit) return k;
    k = k + 1;
  }
}
fn hash(n) {
  let h = 17;
  let i = 0;
  while (i < n) {
    h = h * 31 + i;
    i = i + 1;
  }
  return h;
}
print table_sum(10); // expect: 3025
print first_square_above(1000); // expect: 32
print hash(1000); // expect: 1862049029
