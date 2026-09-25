{
  let a = "outer";
  {
    let b = "middle";
    {
      let c = "inner";
      print a; // expect: outer
      print b; // expect: middle
      print c; // expect: inner
      a = "changed";
    }
  }
  print a; // expect: changed
}
