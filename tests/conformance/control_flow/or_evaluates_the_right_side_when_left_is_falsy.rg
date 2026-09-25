fn side(name, value) {
  print name;
  return value;
}
print side("left", nil) or side("right", "y");
// expect: left
// expect: right
// expect: y
