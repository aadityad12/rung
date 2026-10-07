// A `<` or `<=` test whose result only decides a jump: the register VM can run the test and the
// jump as one instruction (--superinstructions). The output must not depend on it.
fn count_below(n) {
  let i = 0;
  let steps = 0;
  while (i < n) {
    i = i + 1;
    steps = steps + 1;
  }
  return steps;
}

fn count_up_to(n) {
  let i = 0;
  let steps = 0;
  while (i <= n) {
    i = i + 1;
    steps = steps + 1;
  }
  return steps;
}

print count_below(0);  // expect: 0
print count_below(1);  // expect: 1
print count_below(10); // expect: 10
print count_up_to(0);  // expect: 1
print count_up_to(10); // expect: 11
print count_up_to(-1); // expect: 0

let pick = 0;
if (pick < 1) print "lt true"; // expect: lt true
if (pick < 0) print "lt false";
if (pick <= 0) print "le true"; // expect: le true
if (pick <= -1) print "le false";
if (pick < 0) print "a"; else print "b"; // expect: b
