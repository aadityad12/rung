// Every way a program can make a NaN, including a negated one (its sign bit set). A NaN-boxed
// build must store each as a float that prints `nan`, never as a tag (notes D15).
let inf = 1.0 / 0.0;
let a = 0.0 / 0.0;
let b = -a;
let c = inf - inf;
let d = inf * 0;
let e = -(0 / 0.0);
print a; // expect: nan
print b; // expect: nan
print c; // expect: nan
print d; // expect: nan
print e; // expect: nan
print [a, b, c]; // expect: [nan, nan, nan]
print b == b; // expect: false
print b == nil; // expect: false
print b == false; // expect: false
print b == true; // expect: false
print b + 1; // expect: nan
print len([a, b, c, d, e]); // expect: 5
let arr = array(2, b);
print arr[1]; // expect: nan
if (b) print "truthy"; // expect: truthy
