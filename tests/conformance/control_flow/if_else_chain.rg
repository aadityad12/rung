fn classify(n) {
  if (n < 0) return "negative";
  else if (n == 0) return "zero";
  else if (n < 10) return "small";
  else return "large";
}
print classify(-5); // expect: negative
print classify(0); // expect: zero
print classify(7); // expect: small
print classify(100); // expect: large
