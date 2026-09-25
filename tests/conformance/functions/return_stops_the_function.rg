fn f() {
  print "before"; // expect: before
  return 1;
  print "after";
}
print f(); // expect: 1
