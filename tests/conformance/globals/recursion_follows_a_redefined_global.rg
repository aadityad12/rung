// A recursive call goes through the global `f`. After `f` is rebound, the old body's recursive
// call reaches the new function.
fn f(n) {
  if (n == 0) return "done";
  return f(n - 1);
}
print f(3); // expect: done
let old = f;
fn f(n) { return "new f"; }
print old(3); // expect: new f
print f(3); // expect: new f
