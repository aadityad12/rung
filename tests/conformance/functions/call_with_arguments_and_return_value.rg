fn add(a, b) { return a + b; }
print add(1, 2); // expect: 3
print add(add(1, 2), add(3, 4)); // expect: 10
