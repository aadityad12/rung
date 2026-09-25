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

> **Status: under construction.** The lexer is done. Nothing executes yet. Every number that
> ends up in this README will come from a committed file in `results/` produced by a script.
> None are hand-written.

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
cmake --preset debug          # also: release, asan (Address + UB sanitizers), tsan, fuzz
cmake --build --preset debug
ctest --preset debug
./build/debug/rung --dump-tokens examples/hello.rg
./build/debug/rung --dump-ast examples/hello.rg
./build/debug/rung --dump-bytecode examples/hello.rg                    # stack bytecode
./build/debug/rung --dump-bytecode --engine=register examples/hello.rg  # register bytecode
```

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
   [`tests/conformance/README.md`](tests/conformance/README.md).
2. **Unit tests** (`tests/unit/`, [doctest](https://github.com/doctest/doctest)). Each C++
   module on its own: the lexer, parser, resolver, runtime and GC, and the ARM64 encoder
   (checked byte for byte against LLVM). `ctest --preset debug` runs them.
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
`tests/conformance/**/*.rg` and `examples/*.rg`. Inputs are capped at 4096 bytes because very
long flat expression chains (`1+1+...+1`, thousands of terms) overflow the native stack in the
resolver; that open problem, and the options for it, are in [notes Q1](docs/notes.md) (§4).

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

## Design notes

Every design decision, what it replaced, and why, is recorded in
[`docs/notes.md`](docs/notes.md). As each ladder rung lands, the same file records what it was
expected to do, what it measured, and why those differed.

## Acknowledgements

- Robert Nystrom, [*Crafting Interpreters*](https://craftinginterpreters.com/). The tree-walker
  and the stack-based VM follow the structure of that book's `jlox` and `clox` closely:
  single-pass compilation to a `Chunk`, the dispatch loop, and Lua-style upvalues for closures.
  Anything that looks like `clox` probably is.
- Roberto Ierusalimschy, Luiz Henrique de Figueiredo, Waldemar Celes,
  [*The Implementation of Lua 5.0*](https://www.lua.org/doc/jucs05.pdf) (2005). The basis for
  the register-based VM.
- Everything past the register VM (the JIT and background compilation) goes beyond both.

## License

MIT. See [`LICENSE`](LICENSE). Vendored [doctest](https://github.com/doctest/doctest) is also MIT.
