// A float fails an integer guard only at the bottom of four recursive calls.
// The VM resumes that call and completes every suspended caller exactly once.
fn tally(n, step) {
  if (n == 0) {
    let total = 1;
    total = total + 1;
    return total + step;
  }
  return tally(n - 1, step) + 1;
}
print tally(4, 1);
print tally(4, 0.5);
