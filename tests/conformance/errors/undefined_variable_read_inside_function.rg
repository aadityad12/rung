fn f() {
  return missing; // expect runtime error: undefined variable 'missing'
}
print "start"; // expect: start
f();
