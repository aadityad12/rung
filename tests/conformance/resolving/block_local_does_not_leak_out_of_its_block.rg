let a = "global";
{
  let a = "local";
  print a; // expect: local
}
print a; // expect: global
