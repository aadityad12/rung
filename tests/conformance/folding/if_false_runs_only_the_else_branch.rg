if (false) print "then"; else print "else"; // expect: else
if (nil) { print "then"; } else { print "else block"; } // expect: else block
if (false) print "nothing";
print "after"; // expect: after
