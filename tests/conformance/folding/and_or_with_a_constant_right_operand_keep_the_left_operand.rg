fn side(tag) {
  print tag;
  return nil;
}
print side("left") and true; // expect: left
// expect: nil
print side("left") or false; // expect: left
// expect: false
let x = 3;
print x and 7; // expect: 7
print x or 7; // expect: 3
