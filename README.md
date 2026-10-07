# Rung

A small programming language with several execution engines, each faster than the last, and
every speedup measured on its own:

1. a tree-walking interpreter (the honest, slow baseline),
2. a stack-based bytecode VM,
3. an optimization ladder: computed-goto dispatch, NaN-boxing, a register-based VM,
   superinstructions, inline caching, constant folding, each one switchable and measured alone,
4. a baseline JIT that emits ARM64 machine code for hot integer loops,
5. background JIT compilation on a second thread.

The name comes from the results table: each row is one rung of the ladder.

> **Status: under construction.** The tree-walking interpreter, the stack VM, the register VM
> and the baseline JIT (arm64 only) run Rung programs and pass the conformance suite.
> Background compilation is not built yet. Every number
> that ends up in this README will come from a committed file in `results/` produced by a
> script. None are hand-written.

## The language

Deliberately tiny, because every feature has to be built once per engine: 32-bit integers,
64-bit floats, booleans, `nil`, immutable strings, fixed-size arrays, first-class functions with
closures, `let`, `if`, `while`, `for`, `return`, `print`, plus the native functions `array`,
`len` and `clock`.

```
fn fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
print fib(20);
```

The exact rules every engine must agree on (integer wraparound, division by zero, how floats
print, error messages and exit codes) are in [`docs/notes.md`](docs/notes.md), §2.

## Building

Needs CMake 3.25+, Ninja, and Clang with C++20.

```sh
cmake --preset debug          # also: release, asan (Address + UB sanitizers), tsan, fuzz,
                                # release-goto and asan-goto (computed-goto dispatch),
                                # release-goto-nanbox and asan-goto-nanbox (+ NaN-boxed values)
cmake --build --preset debug
ctest --preset debug
./build/debug/rung examples/hello.rg          # runs on the tree-walker (--engine=tree)
./build/debug/rung --engine=stack --stats examples/hello.rg   # stack VM; debug builds also count instructions
./build/debug/rung --engine=register --stats examples/hello.rg   # register VM, same counters
./build/debug/rung --engine=register --inline-cache --stats examples/hello.rg   # globals without hash lookups
./build/debug/rung --engine=register --superinstructions --stats=pairs examples/hello.rg   # fused pairs; which opcode follows which
# The JIT needs an arm64 CPU and NaN-boxed values, so a *-nanbox preset on an arm64 machine:
cmake --preset release-goto-nanbox && cmake --build --preset release-goto-nanbox
./build/release-goto-nanbox/rung --engine=jit --jit-log tests/conformance/jit/loop_sum.rg
#   --jit-log: each compile, rejection and bail-out to stderr; --jit-threshold=N (default 1000):
#   calls plus loop back-edges before a function is compiled
./build/debug/rung --gc-stress --stats examples/hello.rg   # collect on every allocation; heap stats
./build/debug/rung --engine=register --fold --stats examples/hello.rg   # constant folding + dead-code removal
./build/debug/rung --dump-tokens examples/hello.rg
./build/debug/rung --dump-ast examples/hello.rg
./build/debug/rung --dump-bytecode examples/hello.rg                    # stack bytecode
./build/debug/rung --dump-bytecode --engine=register examples/hello.rg  # register bytecode
./build/release/rung --bench=20 --bench-out=out.json bench/fib.rg   # time 20 calls of run()
```

The six benchmark programs live in `bench/`; `--bench=N` runs one program's top level and then
times N calls of its `run()` function in C++, writing the per-call times to the JSON file
(notes D12). `bench/expected.json` holds each benchmark's checksum, which the `release` preset's
`bench-check-*` ctest tests verify on every engine (correctness only, never timing).

CI builds and tests on macOS ARM64, Linux ARM64, and Linux x86-64 (inside the `Dockerfile`).

## Testing

Five layers, each catching something the others cannot:

1. **Conformance tests** (`tests/conformance/<topic>/<name>.rg`). A Rung program with its
   expected output written in `// expect:` comments beside the code (format in
   [notes D13](docs/notes.md)). The runner executes every program on **every engine** and
   fails on any difference, so an engine that disagrees with the others, or with the
   [semantics contract](docs/notes.md), cannot pass. Run it with
   `python3 tests/run_conformance.py --rung build/debug/rung --engine tree [--gc-stress]
   [FILTER...]`; the format and how to add a test are in
   [`tests/conformance/README.md`](tests/conformance/README.md). `ctest` runs it once per
   engine, and once more per engine with `--gc-stress` (`conformance-tree`,
   `conformance-tree-gc-stress`, `conformance-stack`, `conformance-stack-gc-stress`,
   `conformance-register`, `conformance-register-gc-stress`, `conformance-jit`,
   `conformance-jit-gc-stress`), and once more per engine with `--fold`
   (`conformance-<engine>-fold`; the register VM also with `--gc-stress`), since folding must not
   change what any program does, and the register VM once more with `--inline-cache`
   (`conformance-register-inline-cache`, and with `--gc-stress`), and once more with
   `--superinstructions` (`conformance-register-superinstructions`, with `--gc-stress`, and with
   all the register-VM rungs on at once as `conformance-register-all-rungs`). The JIT runs the
   suite with `--jit-threshold=1`, so every function the JIT can compile is compiled on its first
   call, also with `--superinstructions` and with every rung on (`conformance-jit-fold`,
   `conformance-jit-superinstructions`, `conformance-jit-all-rungs`); in a build without the JIT
   they are reported as skipped.
2. **Unit tests** (`tests/unit/`, [doctest](https://github.com/doctest/doctest)). Each C++
   module on its own: the lexer, parser, resolver, runtime and GC, the tree-walker, both
   compilers and both VMs, the ARM64 encoder (checked byte for byte against LLVM), and the
   JIT's whitelist and generated code (run directly on a register file).
   `ctest --preset debug` runs them.
3. **Sanitizers.** The `asan` preset builds everything with AddressSanitizer and
   UndefinedBehaviorSanitizer, and `tsan` with ThreadSanitizer (for the background JIT thread).
   Warnings are errors. Run `asan` before every PR.
4. **Fuzzing.** A coverage-guided libFuzzer target feeds random bytes to the lexer, parser and
   resolver, and must never crash, hang, leak or trigger a sanitizer (details below). It stops
   before execution, because a valid Rung program may loop forever.
5. **Three-platform CI.** Every push builds and tests on macOS ARM64, Linux ARM64 and Linux
   x86-64 (inside the `Dockerfile`, with the JIT disabled), plus the fuzz job below.

### Fuzzing

`fuzz/fuzz_frontend.cpp` is the target; `fuzz/rung.dict` lists Rung's keywords and operators
for the mutator; `fuzz/run_fuzz.sh` runs it for a fixed time. The seed corpus is every
`tests/conformance/**/*.rg` and `examples/*.rg`. Inputs have no size cap: an expression more than
1000 links long (`1+1+...+1`, thousands of terms) is the compile error `expression chain too
long`, so it cannot overflow the native stack (the rule and how it was chosen: notes §2.6 and
[Q1](docs/notes.md)).

**libFuzzer needs a clang that ships its runtime, and Apple clang does not.**

- **Linux** (this is what CI uses): `sudo apt-get install clang cmake ninja-build`, then
  ```sh
  cmake --preset fuzz && cmake --build --preset fuzz
  fuzz/run_fuzz.sh build/fuzz/rung_fuzz_frontend 60
  ```
- **macOS**: install Homebrew LLVM (`brew install llvm`) and point CMake at it:
  ```sh
  cmake --preset fuzz -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++"
  cmake --build --preset fuzz
  fuzz/run_fuzz.sh build/fuzz/rung_fuzz_frontend 60
  ```
  Configuring `fuzz` with Apple clang stops with an error that says so.

`ctest --preset fuzz` replays the seed files once (no fuzzing) as a quick check. A failing run
writes the offending input to `fuzz-artifacts/`; CI uploads that directory as the `fuzz-crash`
artifact. Reproduce with `build/fuzz/rung_fuzz_frontend fuzz-artifacts/crash-<hash>`. Every bug
the fuzzer finds gets fixed and its input added as a unit or conformance test.

## Results

Every number about speed in this repository comes from one pipeline: `scripts/bench.py` measures
the configurations listed in `scripts/ladder_configs.json` and writes `results/<id>.json`;
`scripts/ladder.py` turns those files into the tables below. Nothing here is typed by hand, and
CI runs `python3 scripts/ladder.py --check`, which fails if the tables differ from what the
committed results files generate.

<!-- ladder:start -->
_This section is generated by `scripts/ladder.py` from the files in `results/`. Do not edit it by hand: `ladder.py --check` runs in CI and fails if it differs._

No results have been recorded yet. Each row appears here once `scripts/bench.py` has measured it on the development machine (notes D5).
<!-- ladder:end -->

## How to reproduce

Benchmarks run only on the development machine (notes D5), never in CI. Plug the Mac in, turn
Low Power Mode off, close other apps, and leave it alone while it runs.

```sh
git status                                  # must be clean: results are tied to a commit
python3 scripts/bench.py                    # builds the presets, measures every row
python3 scripts/ladder.py                   # rewrites the table above from results/*.json
python3 scripts/ladder.py --check           # what CI runs
```

`bench.py --configs 01_tree --benchmarks fib sieve --runs 5 --iterations 10` runs a subset. By
default each (configuration, benchmark) pair runs in 11 rounds of a fresh process that calls
`run()` 20 times. In every round all pairs run once, in an order shuffled by a recorded seed, so
background activity lands on every configuration alike. Round 1 is warm-up and discarded; the
other 10 give the median and interquartile range (IQR) of each run's total time, and the p50,
p99 and max over all per-iteration times pooled. A pair whose IQR exceeds 5% of its median is
flagged `noisy` in the results file and in the table; rerun it. Each results file also records
the CPU, macOS version, power source, compiler, git commit, date, run counts and seed, plus peak
memory (RSS) per process. `bench.py` refuses to run on a dirty git tree (`--allow-dirty`
overrides this and records `dirty: true`), aborts if any benchmark returns a result other than
the one in `bench/expected.json`, and prints how a results file changed before replacing it.

## Design notes

Every design decision, what it replaced, and why, is recorded in
[`docs/notes.md`](docs/notes.md). As each ladder rung lands, the same file records what it was
expected to do, what it measured, and why those differed.

## Acknowledgements

- Robert Nystrom, [*Crafting Interpreters*](https://craftinginterpreters.com/). The tree-walker
  and the stack-based VM follow the structure of that book's `jlox` and `clox` closely:
  single-pass compilation to a `Chunk`, the dispatch loop, and Lua-style upvalues for closures.
  Anything that looks like `clox` probably is. The NaN-boxed `Value` follows the book's
  "Optimization" chapter too (quiet-NaN prefix, sign bit marking a pointer), with an int32 tag
  added.
- Roberto Ierusalimschy, Luiz Henrique de Figueiredo, Waldemar Celes,
  [*The Implementation of Lua 5.0*](https://www.lua.org/doc/jucs05.pdf) (2005). The basis for
  the register-based VM.
- Everything past the register VM (the JIT and background compilation) goes beyond both.

## License

MIT. See [`LICENSE`](LICENSE). Vendored [doctest](https://github.com/doctest/doctest) is also MIT.
