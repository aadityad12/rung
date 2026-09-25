fn f() { }
if (true) print "true"; else print "no"; // expect: true
if (1) print "int"; else print "no"; // expect: int
if ("x") print "string"; else print "no"; // expect: string
if (f) print "function"; else print "no"; // expect: function
if (len) print "native"; else print "no"; // expect: native
