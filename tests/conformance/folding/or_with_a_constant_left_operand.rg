fn side(tag) {
  print tag;
  return tag;
}
print false or 5; // expect: 5
print nil or "x"; // expect: x
print 0 or "zero"; // expect: 0
print "" or "empty"; // expect:
print true or side("never"); // expect: true
print 1 or side("never"); // expect: 1
print nil or side("runs"); // expect: runs
// expect: runs
