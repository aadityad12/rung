fn make_counter() {
  let count = 0;
  fn next() {
    count = count + 1;
    return count;
  }
  return next;
}
let a = make_counter();
let b = make_counter();
print a(); // expect: 1
print a(); // expect: 2
print b(); // expect: 1
print a(); // expect: 3
