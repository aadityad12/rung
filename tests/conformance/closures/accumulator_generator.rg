fn make_acc(total) {
  fn add(n) {
    total = total + n;
    return total;
  }
  return add;
}
let acc = make_acc(100);
acc(10);
acc(20);
print acc(0); // expect: 130
