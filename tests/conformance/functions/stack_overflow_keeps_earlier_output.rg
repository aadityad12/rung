fn recurse(n) { return recurse(n + 1); } // expect runtime error: stack overflow
print "started"; // expect: started
recurse(0);
