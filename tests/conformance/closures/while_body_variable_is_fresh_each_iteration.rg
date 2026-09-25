let fns = array(3, nil);
let i = 0;
while (i < 3) {
  let j = i;
  fn get() { return j; }
  fns[i] = get;
  i = i + 1;
}
print fns[0](); // expect: 0
print fns[1](); // expect: 1
print fns[2](); // expect: 2
