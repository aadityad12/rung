fn make(n) {
  let box = [n];
  fn get() { return box[0]; }
  fn put(v) { box[0] = v; }
  return [get, put];
}
let pairs = array(10, nil);
for (let i = 0; i < 10; i = i + 1) pairs[i] = make(i);
for (let i = 0; i < 500; i = i + 1) { let garbage = [i, i, i]; }
let total = 0;
for (let i = 0; i < 10; i = i + 1) {
  pairs[i][1](pairs[i][0]() * 2);
  total = total + pairs[i][0]();
}
print total; // expect: 90
