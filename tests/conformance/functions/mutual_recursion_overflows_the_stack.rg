fn ping(n) { return pong(n + 1); } fn pong(n) { return ping(n + 1); } // expect runtime error: stack overflow
ping(0);
