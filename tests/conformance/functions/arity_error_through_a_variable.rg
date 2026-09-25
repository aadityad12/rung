fn f(a) { }
let g = f;
g(); // expect runtime error: expected 1 arguments but got 0
