let saved;
fn setup() {
  let secret = "hidden";
  fn reveal() { return secret; }
  saved = reveal;
}
setup();
print saved(); // expect: hidden
