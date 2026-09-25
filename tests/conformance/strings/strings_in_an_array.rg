let words = ["a", "bb", "ccc"];
let total = 0;
for (let i = 0; i < len(words); i = i + 1) total = total + len(words[i]);
print total; // expect: 6
print words[0] + words[2]; // expect: accc
