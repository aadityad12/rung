// Run it with `rung examples/counter.rg`. It prints 2: `next` keeps its own `count` alive after
// make_counter returns, because it captured it.
fn make_counter() {
  let count = 0;
  fn next() { count = count + 1; return count; }
  return next;
}
let counter = make_counter();
counter();
print counter();
