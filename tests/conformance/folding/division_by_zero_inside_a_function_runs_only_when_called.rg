// Folding must not move the error to compile time or to the function's definition.
fn boom() {
  return 1 / 0; // expect runtime error: division by zero
}
print "defined"; // expect: defined
print "calling"; // expect: calling
print boom();
