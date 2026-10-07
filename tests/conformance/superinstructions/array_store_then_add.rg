// An array write followed directly by an addition (`flags[j] = false; j = j + i;`).
fn run() {
  let a = array(5, 0);
  let j = 0;
  while (j < 5) {
    a[j] = j * j;
    j = j + 2;
  }
  print a[0]; // expect: 0
  print a[2]; // expect: 4
  print a[4]; // expect: 16
  print a[1]; // expect: 0
  return j;
}
print run(); // expect: 6

// The addition after the store builds a new string, so under --gc-stress it collects while the
// array holds a string the fused instruction has just stored.
fn strings() {
  let words = array(3, "");
  let i = 0;
  let acc = "";
  while (i < 3) {
    words[i] = acc + "x";
    acc = acc + "ab";
    i = i + 1;
  }
  print words[0]; // expect: x
  print words[1]; // expect: abx
  print words[2]; // expect: ababx
  return acc;
}
print strings(); // expect: ababab
