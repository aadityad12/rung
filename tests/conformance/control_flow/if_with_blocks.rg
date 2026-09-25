let x = 3;
if (x > 2) {
  print "big"; // expect: big
  x = x - 1;
} else {
  print "small";
}
print x; // expect: 2
