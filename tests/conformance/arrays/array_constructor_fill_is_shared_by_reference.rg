let inner = [0];
let outer = array(2, inner);
outer[0][0] = 5;
print outer; // expect: [[5], [5]]
