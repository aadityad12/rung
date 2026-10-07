// The fused compare-and-branch must use the same comparison rules as the plain instructions:
// ints and floats compare by value, and NaN is neither less than nor equal to anything.
let nan = 0.0 / 0.0;
let inf = 1.0 / 0.0;
if (1 < 1.5) print "int < float"; // expect: int < float
if (2 <= 2.0) print "int <= float"; // expect: int <= float
if (2.5 < 2) print "wrong"; else print "float !< int"; // expect: float !< int
if (nan < 1) print "wrong"; else print "nan !< 1"; // expect: nan !< 1
if (nan <= nan) print "wrong"; else print "nan !<= nan"; // expect: nan !<= nan
if (1 < inf) print "1 < inf"; // expect: 1 < inf
if (-inf <= -2147483648) print "-inf <= min"; // expect: -inf <= min
if (2147483647 < 2147483647.5) print "max < max+.5"; // expect: max < max+.5
