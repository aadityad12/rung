fn f() {
  if (true) return "certain";
  print "never";
  return "not reached";
}
print f(); // expect: certain
