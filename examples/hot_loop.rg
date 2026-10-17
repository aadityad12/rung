// The first call makes sum hot. With --engine=jit, the second enters machine code.
fn sum(n) {
  let total = 0;
  let i = 0;
  while (i < n) {
    total = (total + i) % 1000007;
    i = i + 1;
  }
  return total;
}
print sum(10000);
print sum(10000);
