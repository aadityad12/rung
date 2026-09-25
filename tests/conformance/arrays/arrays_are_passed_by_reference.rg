fn fill(a, v) {
  for (let i = 0; i < len(a); i = i + 1) a[i] = v;
}
let a = array(3, 0);
fill(a, 7);
print a; // expect: [7, 7, 7]
