// An operand keeps the value it had when it was evaluated, even if a later operand of the same
// operator assigns the variable. The register VM reads locals in place, so it must copy them.
{
  let a = 1;
  print a + (a = 5); // expect: 6
  print a; // expect: 5
  let b = 2;
  print b * (b = 3) * b; // expect: 18
  let c = 10;
  c = (c + 1) and c;
  print c; // expect: 10
  let xs = [1, 2];
  print xs[0] + (xs[0] = 7); // expect: 8
  print xs; // expect: [7, 2]
}
let g = 1;
print g + (g = 5); // expect: 6
