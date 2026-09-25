let n = 30;
let is_prime = array(n + 1, true);
is_prime[0] = false;
is_prime[1] = false;
for (let i = 2; i * i <= n; i = i + 1) {
  if (is_prime[i]) {
    for (let j = i * i; j <= n; j = j + i) is_prime[j] = false;
  }
}
let primes = [];
let count = 0;
for (let i = 0; i <= n; i = i + 1) if (is_prime[i]) count = count + 1;
print count; // expect: 10
let last = 0;
for (let i = 0; i <= n; i = i + 1) if (is_prime[i]) last = i;
print last; // expect: 29
