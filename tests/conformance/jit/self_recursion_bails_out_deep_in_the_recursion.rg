// A float reaches a `+` only at the bottom of the recursion, three self-calls deep in machine
// code. The VM must resume that innermost call exactly at the failing `+`: resuming any earlier
// would add 1 to `s` again. The callers then finish in the VM from just after their calls.
fn tally(n, step) {
  if (n == 0) {
    let s = 1;
    s = s + 1;
    s = s + 1;
    s = s + step;
    return s;
  }
  return tally(n - 1, step) + 1;
}
print tally(3, 1); // expect: 7
print tally(3, 0.5); // expect: 6.5
print tally(3, 1); // expect: 7
