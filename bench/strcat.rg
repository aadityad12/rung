// Allocation and GC pressure: every `+` on strings makes a new interned string (notes D2, D10),
// and almost all of them become garbage at once. Builds many short strings and one long one.
fn run() {
  let total = 0;
  let i = 0;
  while (i < 30000) {
    let s = "";
    let j = 0;
    while (j < 10) {
      s = s + "ab";
      j = j + 1;
    }
    total = total + len(s);
    i = i + 1;
  }
  let big = "";
  let k = 0;
  while (k < 4000) {
    big = big + "xyz";
    k = k + 1;
  }
  return total + len(big);
}
