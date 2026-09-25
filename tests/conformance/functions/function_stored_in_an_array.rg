fn one() { return 1; }
fn two() { return 2; }
let fs = [one, two];
print fs[0]() + fs[1](); // expect: 3
