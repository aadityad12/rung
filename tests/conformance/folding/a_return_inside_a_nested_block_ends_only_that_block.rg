fn f() {
  {
    return "inner";
    print "dead";
  }
  print "unreachable too";
}
print f(); // expect: inner
