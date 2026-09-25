let m = [[1, 2], [3, 4]];
m[1][0] = 30;
print m; // expect: [[1, 2], [30, 4]]
print m[0][1] + m[1][1]; // expect: 6
