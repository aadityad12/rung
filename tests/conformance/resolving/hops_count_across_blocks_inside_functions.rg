fn f() {
  let a = 1;
  {
    let b = 2;
    {
      fn g() { return a + b; }
      return g();
    }
  }
}
print f(); // expect: 3
