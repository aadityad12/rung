if (1 < 2 and 3 > 2) print "yes"; else print "no"; // expect: yes
if (1 > 2 or 2 > 3) print "yes"; else print "no"; // expect: no
let i = 0;
while (i < 2 + 1) {
  print i; // expect: 0
  // expect: 1
  // expect: 2
  i = i + 1;
}
