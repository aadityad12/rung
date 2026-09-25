{
  let a = (a = 1); // expect compile error: can't read local variable 'a' in its own initializer
}
