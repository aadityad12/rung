let s = "";
for (let i = 0; i < 200; i = i + 1) s = s + "ab";
print len(s); // expect: 400
let parts = array(50, nil);
for (let i = 0; i < 50; i = i + 1) parts[i] = "n" + "x";
let total = 0;
for (let i = 0; i < 50; i = i + 1) total = total + len(parts[i]);
print total; // expect: 100
