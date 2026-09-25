let shared = "start";
fn a() { shared = shared + " a"; }
fn b() { shared = shared + " b"; }
a();
b();
print shared; // expect: start a b
