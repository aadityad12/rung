// bench/sieve.rg's loops in a function that is given the array instead of creating it. `mark`
// reads and writes elements and nothing else, so it compiles; `count_primes` calls the native
// `array` (a global and a call that allocates) and stays in the VM.
fn mark(flags, n) {
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
fn count_primes(n) {
  return mark(array(n + 1, true), n);
}
print count_primes(1); // expect: 0
print count_primes(2); // expect: 1
print count_primes(100); // expect: 25
print count_primes(5000); // expect: 669
