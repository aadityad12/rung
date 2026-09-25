fn side(name, value) {
  print name;
  return value;
}
print side("left", "x") or side("right", "y");
// expect: left
// expect: x
