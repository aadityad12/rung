if (true) print "then"; else print "else"; // expect: then
if (1) { print "block"; } else { print "else"; } // expect: block
if (0) print "zero is truthy"; else print "else"; // expect: zero is truthy
if ("") print "empty is truthy"; else print "else"; // expect: empty is truthy
