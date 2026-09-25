fn side(name, value) {
  print name;
  return value;
}
print side("left", 1) and side("right", 2);
// expect: left
// expect: right
// expect: 2
