fn bump(x) {
  x = x + 1;
  return x;
}
let a = 1;
print bump(a); // expect: 2
print a; // expect: 1
