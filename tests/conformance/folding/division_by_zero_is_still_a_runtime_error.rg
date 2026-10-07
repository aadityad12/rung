// A constant division by zero is not folded, so the error happens when the statement runs.
print "before"; // expect: before
print 1 / 0; // expect runtime error: division by zero
print "after";
