// WARM-UP BENCHMARK (notes D8, issue #27). Not part of the speedup ladder: it exists to make a
// JIT compile pause visible per call of run(), so that compiling synchronously and compiling on
// the background thread (--jit-background) can be compared on p50, p99 and max per call.
//
// What makes a pause visible here:
// - Many distinct hot functions: 64, in four shapes (16 each) with different constants. Every
//   one is inside the JIT's whitelist (int arithmetic, comparisons, jumps and returns only;
//   notes D16), so every one is compiled.
// - All 64 become hot in the same call of run(), and not the first one. Each call of a function
//   adds 1 to its hotness, and each of its 249 loop back-edges adds 1 more: 250 per call. At the
//   default --jit-threshold=1000 the count reaches the threshold at the end of the 4th call, so
//   calls 1 to 4 of run() are interpreted, call 4 is also the one in which 64 compiles happen
//   (synchronously, they pause it; in the background, they do not), and calls 5 on run machine
//   code. Keeping the compiles out of call 1 keeps them apart from first-call effects (cold
//   caches, page faults). tests/run_jit_coverage_check.py checks this timing: nothing compiles
//   in 3 calls, all 64 in 4.
// - Short calls: each run() is 64 calls of a 249-step loop, so a pause of a few microseconds per
//   compile is a large share of one call if it is real.
// run() itself calls globals, so the JIT rejects it and it stays in the VM, as a driver. No
// value wraps (notes D1): every function keeps its numbers small with `%`.

fn mix_00(n) {
  let s = 1;
  let i = 0;
  while (i < n) {
    s = (s * 3 + i) % 10007;
    i = i + 1;
  }
  return s;
}

fn mix_01(n) {
  let s = 2;
  let i = 0;
  while (i < n) {
    s = (s * 5 + i) % 10009;
    i = i + 1;
  }
  return s;
}

fn mix_02(n) {
  let s = 3;
  let i = 0;
  while (i < n) {
    s = (s * 7 + i) % 10037;
    i = i + 1;
  }
  return s;
}

fn mix_03(n) {
  let s = 4;
  let i = 0;
  while (i < n) {
    s = (s * 9 + i) % 10039;
    i = i + 1;
  }
  return s;
}

fn mix_04(n) {
  let s = 5;
  let i = 0;
  while (i < n) {
    s = (s * 11 + i) % 10061;
    i = i + 1;
  }
  return s;
}

fn mix_05(n) {
  let s = 6;
  let i = 0;
  while (i < n) {
    s = (s * 13 + i) % 10067;
    i = i + 1;
  }
  return s;
}

fn mix_06(n) {
  let s = 7;
  let i = 0;
  while (i < n) {
    s = (s * 15 + i) % 10069;
    i = i + 1;
  }
  return s;
}

fn mix_07(n) {
  let s = 8;
  let i = 0;
  while (i < n) {
    s = (s * 17 + i) % 10079;
    i = i + 1;
  }
  return s;
}

fn mix_08(n) {
  let s = 9;
  let i = 0;
  while (i < n) {
    s = (s * 19 + i) % 10091;
    i = i + 1;
  }
  return s;
}

fn mix_09(n) {
  let s = 10;
  let i = 0;
  while (i < n) {
    s = (s * 21 + i) % 10093;
    i = i + 1;
  }
  return s;
}

fn mix_10(n) {
  let s = 11;
  let i = 0;
  while (i < n) {
    s = (s * 23 + i) % 10099;
    i = i + 1;
  }
  return s;
}

fn mix_11(n) {
  let s = 12;
  let i = 0;
  while (i < n) {
    s = (s * 25 + i) % 10103;
    i = i + 1;
  }
  return s;
}

fn mix_12(n) {
  let s = 13;
  let i = 0;
  while (i < n) {
    s = (s * 27 + i) % 10111;
    i = i + 1;
  }
  return s;
}

fn mix_13(n) {
  let s = 14;
  let i = 0;
  while (i < n) {
    s = (s * 29 + i) % 10133;
    i = i + 1;
  }
  return s;
}

fn mix_14(n) {
  let s = 15;
  let i = 0;
  while (i < n) {
    s = (s * 31 + i) % 10139;
    i = i + 1;
  }
  return s;
}

fn mix_15(n) {
  let s = 16;
  let i = 0;
  while (i < n) {
    s = (s * 33 + i) % 10141;
    i = i + 1;
  }
  return s;
}

fn pair_00(n) {
  let a = 0;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10141;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_01(n) {
  let a = 1;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10139;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_02(n) {
  let a = 2;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10133;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_03(n) {
  let a = 3;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10111;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_04(n) {
  let a = 4;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10103;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_05(n) {
  let a = 5;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10099;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_06(n) {
  let a = 6;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10093;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_07(n) {
  let a = 7;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10091;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_08(n) {
  let a = 8;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10079;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_09(n) {
  let a = 9;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10069;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_10(n) {
  let a = 10;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10067;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_11(n) {
  let a = 11;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10061;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_12(n) {
  let a = 12;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10039;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_13(n) {
  let a = 13;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10037;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_14(n) {
  let a = 14;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10009;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn pair_15(n) {
  let a = 15;
  let b = 1;
  let i = 0;
  while (i < n) {
    let t = (a + b) % 10007;
    a = b;
    b = t;
    i = i + 1;
  }
  return b;
}

fn count_00(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 3 == 0) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_01(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 4 == 1) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_02(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 5 == 2) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_03(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 6 == 3) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_04(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 7 == 4) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_05(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 8 == 5) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_06(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 9 == 6) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_07(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 10 == 7) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_08(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 11 == 8) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_09(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 12 == 9) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_10(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 13 == 10) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_11(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 14 == 11) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_12(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 15 == 12) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_13(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 16 == 13) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_14(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 17 == 14) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn count_15(n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    if (i % 18 == 15) s = s + i; else s = s - 1;
    i = i + 1;
  }
  return s % 1009;
}

fn hail_00(n) {
  let x = 7;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 11;
    i = i + 1;
  }
  return c;
}

fn hail_01(n) {
  let x = 13;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 12;
    i = i + 1;
  }
  return c;
}

fn hail_02(n) {
  let x = 19;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 13;
    i = i + 1;
  }
  return c;
}

fn hail_03(n) {
  let x = 25;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 14;
    i = i + 1;
  }
  return c;
}

fn hail_04(n) {
  let x = 31;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 15;
    i = i + 1;
  }
  return c;
}

fn hail_05(n) {
  let x = 37;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 16;
    i = i + 1;
  }
  return c;
}

fn hail_06(n) {
  let x = 43;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 17;
    i = i + 1;
  }
  return c;
}

fn hail_07(n) {
  let x = 49;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 18;
    i = i + 1;
  }
  return c;
}

fn hail_08(n) {
  let x = 55;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 19;
    i = i + 1;
  }
  return c;
}

fn hail_09(n) {
  let x = 61;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 20;
    i = i + 1;
  }
  return c;
}

fn hail_10(n) {
  let x = 67;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 21;
    i = i + 1;
  }
  return c;
}

fn hail_11(n) {
  let x = 73;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 22;
    i = i + 1;
  }
  return c;
}

fn hail_12(n) {
  let x = 79;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 23;
    i = i + 1;
  }
  return c;
}

fn hail_13(n) {
  let x = 85;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 24;
    i = i + 1;
  }
  return c;
}

fn hail_14(n) {
  let x = 91;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 25;
    i = i + 1;
  }
  return c;
}

fn hail_15(n) {
  let x = 97;
  let c = 0;
  let i = 0;
  while (i < n) {
    if (x % 2 == 0) x = x / 2; else x = 3 * x + 1;
    c = c + x % 26;
    i = i + 1;
  }
  return c;
}

fn run() {
  let n = 249;
  let sum = 0;
  sum = (sum + mix_00(n)) % 1000003;
  sum = (sum + mix_01(n)) % 1000003;
  sum = (sum + mix_02(n)) % 1000003;
  sum = (sum + mix_03(n)) % 1000003;
  sum = (sum + mix_04(n)) % 1000003;
  sum = (sum + mix_05(n)) % 1000003;
  sum = (sum + mix_06(n)) % 1000003;
  sum = (sum + mix_07(n)) % 1000003;
  sum = (sum + mix_08(n)) % 1000003;
  sum = (sum + mix_09(n)) % 1000003;
  sum = (sum + mix_10(n)) % 1000003;
  sum = (sum + mix_11(n)) % 1000003;
  sum = (sum + mix_12(n)) % 1000003;
  sum = (sum + mix_13(n)) % 1000003;
  sum = (sum + mix_14(n)) % 1000003;
  sum = (sum + mix_15(n)) % 1000003;
  sum = (sum + pair_00(n)) % 1000003;
  sum = (sum + pair_01(n)) % 1000003;
  sum = (sum + pair_02(n)) % 1000003;
  sum = (sum + pair_03(n)) % 1000003;
  sum = (sum + pair_04(n)) % 1000003;
  sum = (sum + pair_05(n)) % 1000003;
  sum = (sum + pair_06(n)) % 1000003;
  sum = (sum + pair_07(n)) % 1000003;
  sum = (sum + pair_08(n)) % 1000003;
  sum = (sum + pair_09(n)) % 1000003;
  sum = (sum + pair_10(n)) % 1000003;
  sum = (sum + pair_11(n)) % 1000003;
  sum = (sum + pair_12(n)) % 1000003;
  sum = (sum + pair_13(n)) % 1000003;
  sum = (sum + pair_14(n)) % 1000003;
  sum = (sum + pair_15(n)) % 1000003;
  sum = (sum + count_00(n)) % 1000003;
  sum = (sum + count_01(n)) % 1000003;
  sum = (sum + count_02(n)) % 1000003;
  sum = (sum + count_03(n)) % 1000003;
  sum = (sum + count_04(n)) % 1000003;
  sum = (sum + count_05(n)) % 1000003;
  sum = (sum + count_06(n)) % 1000003;
  sum = (sum + count_07(n)) % 1000003;
  sum = (sum + count_08(n)) % 1000003;
  sum = (sum + count_09(n)) % 1000003;
  sum = (sum + count_10(n)) % 1000003;
  sum = (sum + count_11(n)) % 1000003;
  sum = (sum + count_12(n)) % 1000003;
  sum = (sum + count_13(n)) % 1000003;
  sum = (sum + count_14(n)) % 1000003;
  sum = (sum + count_15(n)) % 1000003;
  sum = (sum + hail_00(n)) % 1000003;
  sum = (sum + hail_01(n)) % 1000003;
  sum = (sum + hail_02(n)) % 1000003;
  sum = (sum + hail_03(n)) % 1000003;
  sum = (sum + hail_04(n)) % 1000003;
  sum = (sum + hail_05(n)) % 1000003;
  sum = (sum + hail_06(n)) % 1000003;
  sum = (sum + hail_07(n)) % 1000003;
  sum = (sum + hail_08(n)) % 1000003;
  sum = (sum + hail_09(n)) % 1000003;
  sum = (sum + hail_10(n)) % 1000003;
  sum = (sum + hail_11(n)) % 1000003;
  sum = (sum + hail_12(n)) % 1000003;
  sum = (sum + hail_13(n)) % 1000003;
  sum = (sum + hail_14(n)) % 1000003;
  sum = (sum + hail_15(n)) % 1000003;
  return sum;
}
