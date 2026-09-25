// The algorithm in notes 2.3 stops at the first precision that reads back exactly, and %g
// switches to exponent form once the exponent is >= that precision. So 100.0 is "1e+02":
// a consequence of the specified algorithm, pinned here so no engine "fixes" it alone.
print 100.0; // expect: 1e+02
print 10.0; // expect: 1e+01
print 120.0; // expect: 1.2e+02
