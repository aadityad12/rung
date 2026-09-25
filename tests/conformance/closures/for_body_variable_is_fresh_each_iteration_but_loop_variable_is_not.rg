let inner = array(3, nil);
let shared = array(3, nil);
for (let i = 0; i < 3; i = i + 1) {
  let copy = i * 10;
  fn get_copy() { return copy; }
  fn get_i() { return i; }
  inner[i] = get_copy;
  shared[i] = get_i;
}
print inner[0](); // expect: 0
print inner[1](); // expect: 10
print inner[2](); // expect: 20
print shared[0](); // expect: 3
print shared[2](); // expect: 3
