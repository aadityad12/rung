let a = [1];
a[0] = a;
print a[0] == a; // expect: true
print len(a); // expect: 1
