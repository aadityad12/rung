let x = "global";
fn f() {
  let x = "local";
  return x;
}
print f(); // expect: local
print x; // expect: global
