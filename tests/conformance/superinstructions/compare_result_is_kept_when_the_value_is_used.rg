// `a < b and c` is a value: when the test fails, the false it produced is the result. A fused
// compare-and-branch must still write that false to its register.
let a = 3;
let b = 2;
let r = a < b and "yes";
print r; // expect: false
let s = b < a and "yes";
print s; // expect: yes
let t = a <= b or "fallback";
print t; // expect: fallback

fn f(x, y) {
  let ok = x <= y and x < 100;
  return ok;
}
print f(1, 2);   // expect: true
print f(3, 2);   // expect: false
print f(1, 100); // expect: true
print f(150, 200); // expect: false
