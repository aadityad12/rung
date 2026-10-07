fn f() {
  print "one"; // expect: one
  return 1;
  print "two";
  let x = 3;
  return 4;
}
print f(); // expect: 1
