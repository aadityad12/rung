print 1 or 2; // expect: 1
print nil or 2; // expect: 2
print false or "fallback"; // expect: fallback
print 0 or 1; // expect: 0
print nil or false; // expect: false
print false or nil; // expect: nil
