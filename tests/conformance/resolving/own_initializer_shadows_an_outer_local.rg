{
  let a = 1;
  {
    let a = a; // expect compile error: can't read local variable 'a' in its own initializer
  }
}
