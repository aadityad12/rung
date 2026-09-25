fn side(name, value) {
  print name;
  return value;
}
print side("left", false) and side("right", true); // expect: left
// expect: false
