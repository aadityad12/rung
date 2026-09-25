let inf = 1.0 / 0.0;
print inf + 1; // expect: inf
print inf - inf; // expect: nan
print inf > 2147483647; // expect: true
print -inf < 0; // expect: true
