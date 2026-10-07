let x = true;
if (x) if (false) print "no"; else print "else"; // expect: else
while (x) { if (false) print "no"; x = false; }
print "done"; // expect: done
