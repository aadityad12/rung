fn f(c) {
  if (c) return 1 / 0;
  return "ok";
}
print f(false); // expect: ok
