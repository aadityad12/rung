// Notes 2.6: `for` is desugared to `while`, so there is one `i` and every closure sees its
// final value, 3.
let fns = array(3, nil);
for (let i = 0; i < 3; i = i + 1) {
  fn get() { return i; }
  fns[i] = get;
}
print fns[0](); // expect: 3
print fns[1](); // expect: 3
print fns[2](); // expect: 3
