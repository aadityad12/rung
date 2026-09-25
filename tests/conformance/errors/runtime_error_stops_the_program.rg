fn boom() { return 1 / 0; } // expect runtime error: division by zero
print "before"; // expect: before
boom();
print "after";
