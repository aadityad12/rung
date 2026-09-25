let rows = array(3, nil);
for (let i = 0; i < 3; i = i + 1) {
  rows[i] = array(3, 0);
  for (let j = 0; j < 3; j = j + 1) rows[i][j] = i * 3 + j;
}
print rows; // expect: [[0, 1, 2], [3, 4, 5], [6, 7, 8]]
