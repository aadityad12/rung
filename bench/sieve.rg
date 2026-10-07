// Memory access and locals: Sieve of Eratosthenes over an array of bools (notes D2). The inner
// loop is one array write per step, and the outer loop one array read, so it measures indexing.
fn run() {
  let n = 600000;
  let flags = array(n + 1, true);
  flags[0] = false;
  flags[1] = false;
  let i = 2;
  while (i * i <= n) {
    if (flags[i]) {
      let j = i * i;
      while (j <= n) {
        flags[j] = false;
        j = j + i;
      }
    }
    i = i + 1;
  }
  let count = 0;
  let k = 0;
  while (k <= n) {
    if (flags[k]) count = count + 1;
    k = k + 1;
  }
  return count;
}
