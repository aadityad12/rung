fn f() {
  return "kept";
  fn g() { return "dead"; }
  while (true) { print "never"; }
  { print "never"; }
}
print f(); // expect: kept
