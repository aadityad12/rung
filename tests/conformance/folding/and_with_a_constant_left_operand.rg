fn side(tag) {
  print tag;
  return tag;
}
print true and 5; // expect: 5
print 0 and "zero"; // expect: zero
print "" and "empty"; // expect: empty
print false and side("never"); // expect: false
print nil and side("never"); // expect: nil
print true and side("runs"); // expect: runs
// expect: runs
