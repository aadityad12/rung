fn init() {
  print "init"; // expect: init
  return 0;
}
for (let i = init(); false; i = i + 1) print "never";
print "after"; // expect: after
