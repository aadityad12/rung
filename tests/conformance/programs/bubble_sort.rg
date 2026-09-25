fn sort(a) {
  let n = len(a);
  for (let i = 0; i < n; i = i + 1) {
    for (let j = 0; j < n - 1 - i; j = j + 1) {
      if (a[j] > a[j + 1]) {
        let tmp = a[j];
        a[j] = a[j + 1];
        a[j + 1] = tmp;
      }
    }
  }
  return a;
}
print sort([5, 2, 9, 1, 5, 6]); // expect: [1, 2, 5, 5, 6, 9]
print sort([]); // expect: []
print sort([3, 3.5, 2]); // expect: [2, 3, 3.5]
