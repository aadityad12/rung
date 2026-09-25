let x = "global";
fn f(x) { return x; }
print f("param"); // expect: param
print x; // expect: global
