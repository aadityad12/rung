let keep = array(20, nil);
for (let i = 0; i < 2000; i = i + 1) {
  let temp = [i, [i], "s" + "t"];
  keep[i % 20] = temp;
}
print keep[0][0]; // expect: 1980
print keep[19][1][0]; // expect: 1999
