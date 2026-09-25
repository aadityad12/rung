fn make_counter() {
  let count = 0;
  fn next() {
    count = count + 1;
    return count;
  }
  return next;
}
let c = make_counter();
print c(); // expect: 1
print c(); // expect: 2
print c(); // expect: 3
