fn pick(which) {
  fn a() { return "a"; }
  fn b() { return "b"; }
  if (which) return a;
  return b;
}
print pick(true)(); // expect: a
print pick(false)(); // expect: b
