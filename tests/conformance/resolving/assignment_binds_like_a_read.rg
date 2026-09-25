let a = "global";
{
  fn set() { a = "set by closure"; }
  let a = "local";
  set();
  print a; // expect: local
}
print a; // expect: set by closure
