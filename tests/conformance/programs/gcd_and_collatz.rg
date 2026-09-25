fn gcd(a, b) {
  while (b != 0) {
    let t = b;
    b = a % b;
    a = t;
  }
  return a;
}
print gcd(48, 18); // expect: 6
print gcd(17, 5); // expect: 1
fn collatz_steps(n) {
  let steps = 0;
  while (n != 1) {
    if (n % 2 == 0) n = n / 2; else n = 3 * n + 1;
    steps = steps + 1;
  }
  return steps;
}
print collatz_steps(27); // expect: 111
