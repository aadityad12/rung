fn make_pair() {
  let value = 0;
  fn get() { return value; }
  fn set(v) { value = v; }
  let pair = [get, set];
  return pair;
}
let p = make_pair();
let get = p[0];
let set = p[1];
print get(); // expect: 0
set(42);
print get(); // expect: 42
set("changed");
print get(); // expect: changed
