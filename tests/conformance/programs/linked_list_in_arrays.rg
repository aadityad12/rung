let head = nil;
for (let i = 1; i <= 5; i = i + 1) head = [i, head];
let sum = 0;
let node = head;
while (node) {
  sum = sum + node[0];
  node = node[1];
}
print sum; // expect: 15
print head; // expect: [5, [4, [3, [2, [1, nil]]]]]
