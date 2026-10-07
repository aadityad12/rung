{
  let a = "outer";
  if (true) {
    print a; // expect: outer
    let b = "inner";
    { print a; print b; } // expect: outer
    // expect: inner
  } else {
    print "never";
  }
  print a; // expect: outer
}
