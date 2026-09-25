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
cmake --preset debug          # also: release, asan (Address + UB sanitizers), tsan
cmake --build --preset debug
ctest --preset debug
./build/debug/rung --dump-tokens examples/hello.rg
./build/debug/rung --dump-ast examples/hello.rg
./build/debug/rung --dump-bytecode examples/hello.rg
```

CI builds and tests on macOS ARM64, Linux ARM64, and Linux x86-64 (inside the `Dockerfile`).

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
