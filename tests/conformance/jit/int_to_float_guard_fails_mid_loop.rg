// `s` is an int for three iterations; then `s + a` with a float `a` fails the JIT's type guard in
// the middle of the loop. The VM resumes at that ADD with s and i exactly as the machine code
// left them, and finishes the loop with s a float.
fn f(a, n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i == 3) s = s + a; else s = s + 1;
    i = i + 1;
  }
  return s;
}
print f(1, 6); // expect: 6
print f(0.5, 6); // expect: 5.5
print f(2, 6); // expect: 7
