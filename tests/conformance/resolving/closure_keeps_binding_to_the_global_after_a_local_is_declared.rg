// The example from notes D11: a dynamic environment lookup prints "global" then "block".
let a = "global";
{
  fn show() { print a; }
  show(); // expect: global
  let a = "block";
  show(); // expect: global
}
