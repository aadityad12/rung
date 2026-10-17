# Testing Rung

[Back to Rung](../README.md)

## Testing

Five layers, each catching something the others can't:

1. **Conformance tests** (`tests/conformance/`). Rung programs with their expected output written
   in `// expect:` comments. The runner executes every program on every engine and fails on any
   difference. `ctest` runs the suite on every engine, again with garbage collection forced on
   every allocation, and again with each optimization flag and all of them at once. The JIT runs
   it with a threshold of 1, so every function it can compile is compiled on its first call. How
   to add a test is in [`tests/conformance/README.md`](../tests/conformance/README.md).
2. **Unit tests** (`tests/unit/`, [doctest](https://github.com/doctest/doctest)). Every module on
   its own, including the ARM64 encoder against the LLVM assembler's encodings and the
   background compiler's handoff, garbage-collection rooting and shutdown.
3. **Sanitizers.** `asan` builds everything with AddressSanitizer and UndefinedBehaviorSanitizer,
   `tsan` with ThreadSanitizer, and `tsan-goto-nanbox` is ThreadSanitizer with the JIT and its
   compiler thread. Warnings are errors.
4. **Fuzzing.** A libFuzzer target feeds random bytes to the lexer, parser and resolver. It found
   that a long flat chain like `1 + 1 + ... + 1` could overflow the native stack. Such chains are
   now a compile error past 1000 links.
5. **CI on three platforms.** Every push builds and tests on macOS ARM64, Linux ARM64 and Linux
   x86-64 (in Docker, without the JIT), runs the fuzzer, and checks that the README tables match
   the committed results. CI checks correctness only; it never measures speed.

<details>
<summary>Running the fuzzer</summary>

`fuzz/fuzz_frontend.cpp` is the target, `fuzz/rung.dict` lists the language's keywords and
operators for the mutator, and the seed corpus is every conformance test and example. libFuzzer
needs a Clang that ships its runtime, which Apple Clang doesn't.

- **Linux** (what CI uses): `cmake --preset fuzz && cmake --build --preset fuzz`, then
  `fuzz/run_fuzz.sh build/fuzz/rung_fuzz_frontend 60`.
- **macOS**: install Homebrew LLVM, then
  `cmake --preset fuzz -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++"`, build, and run
  the same script.

A failing run writes the input to `fuzz-artifacts/`, and CI uploads it. Every bug the fuzzer
finds gets fixed and its input becomes a test.
</details>

