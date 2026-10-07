fn make() {
  let n = 0;
  if (true) {
    let step = 2;
    fn bump() {
      n = n + step;
      return n;
      print "dead";
    }
    return bump;
  }
}
let bump = make();
print bump(); // expect: 2
print bump(); // expect: 4
