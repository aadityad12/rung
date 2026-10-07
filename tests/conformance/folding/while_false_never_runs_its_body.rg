while (false) print "never";
while (nil) { print "never"; }
while (1 > 2) { print "never"; }
print "after"; // expect: after
