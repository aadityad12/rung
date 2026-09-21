// Rung has no engine yet; `rung --dump-tokens examples/hello.rg` shows what the lexer sees.
fn add(a, b) {
  return a + b;
}

let xs = [1, 2, 3];
print add(xs[0], 2.5);
print "done\n";
