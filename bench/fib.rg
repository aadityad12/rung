// Call overhead, frames and int math. fib calls itself through the global `fib`, which is what
// a real program does (notes D7), so each call is a global lookup plus a call.
fn fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}

fn run() {
  return fib(27);
}
