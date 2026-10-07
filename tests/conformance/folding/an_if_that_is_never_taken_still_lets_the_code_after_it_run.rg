fn f() {
  if (false) return "no";
  print "reached"; // expect: reached
  return "yes";
}
print f(); // expect: yes
