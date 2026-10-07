// A remainder followed directly by an addition: the `% m` idiom that keeps a sum in range.
let sum = 0;
let i = 0;
while (i < 10) {
  sum = (sum + i % 7) % 11;
  i = i + 1;
}
print sum; // expect: 2

print -7 % 3;       // expect: -1
let m = 7 % 4; let n = m + 1;
print n;            // expect: 4
let big = 2147483647 % 5; let wrapped = big + 2147483647;
print wrapped;      // expect: -2147483647
