fn early() { return late(); }
fn late() { return "late is defined"; }
print early(); // expect: late is defined
