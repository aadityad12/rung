// With --superinstructions the loop below is three fused pairs (LT + JUMP_IF_FALSE, MOD + ADD,
// ADD + JUMP). The JIT compiles each word as the instruction it was before fusion, so a guard can
// fail on either half: a float `a` fails the ADD in the second half of MOD + ADD (the VM resumes
// at that word and runs it as a plain ADD), and a float `n` fails the LT in the first half of
// LT + JUMP_IF_FALSE (the VM resumes at the fused word and runs both halves).
fn f(a, d, n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    s = i % d + a;
    i = i + 1;
  }
  return s;
}
print f(2, 5, 6); // expect: 2
print f(0.5, 5, 6); // expect: 0.5
print f(2, 4, 6.5); // expect: 4
print f(1, 3, 4); // expect: 1
