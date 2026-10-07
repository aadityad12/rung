let a = 0;
fn bump() {
  a = a + 1;
  b = 1; // expect runtime error: undefined variable 'b'
}
bump();
