let g;
{
  let x = "from the block";
  fn get() { return x; }
  g = get;
}
print g(); // expect: from the block
