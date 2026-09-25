fn f() {
  let a = 1 + a; // expect compile error: can't read local variable 'a' in its own initializer
}
