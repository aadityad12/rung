// The comparison fails, so the jump after it never runs: the error is on the comparison's line.
let x = nil;
print "before"; // expect: before
if (1 < // expect runtime error: operands must be numbers
    x) print "unreachable";
