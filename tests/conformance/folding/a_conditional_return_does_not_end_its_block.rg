fn f(c) {
  if (c) return "early";
  print "after the if";
  return "late";
}
print f(true); // expect: early
print f(false); // expect: after the if
// expect: late
