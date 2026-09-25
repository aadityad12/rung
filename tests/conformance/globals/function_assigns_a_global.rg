let counter = 0;
fn bump() { counter = counter + 1; }
bump();
bump();
print counter; // expect: 2
