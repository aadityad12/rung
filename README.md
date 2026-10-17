# Rung

[![CI](https://github.com/aadityad12/rung/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/aadityad12/rung/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**One small language. A ladder from tree-walking to handwritten ARM64 machine code.**

Rung is a dynamically typed language built in C++20 to answer a specific question: how much
is each classic interpreter optimization actually worth? The same programs run through a
tree-walker, two bytecode VMs, and an ARM64 JIT with optional background compilation. Every
configuration must preserve the output; the benchmark ladder measures what changes in speed.

**[Run the demo](#try-it)** · **[How it works](docs/architecture.md)** ·
**[Results](#results)** · **[Build & install](docs/install.md)** · **[CLI reference](docs/cli.md)**

## Watch it run

A hot function becomes machine code. A float then fails an integer type guard, and the VM
finishes the call with the right answer.

![Rung compiles ARM64 code, then safely falls back to the VM for a float](docs/assets/jit.gif)

Real command output, generated automatically. Playback is paced for reading; compiler durations
are omitted. [Still image](docs/assets/jit.png) · [Text transcript](docs/assets/demo-transcript.md)

<details>
<summary><strong>Same program across five configurations</strong></summary>

![All five execution configurations agree on the result](docs/assets/engines.gif)

The tour checks both answers on every available configuration. Background compilation may
finish before or after a call; either schedule must preserve the output.
[Example source](examples/hot_loop.rg) · [Still image](docs/assets/engines.png)

</details>

<details>
<summary><strong>Stack versus register bytecode</strong></summary>

![The same addition in stack and register bytecode](docs/assets/bytecode.gif)

The stack VM pushes its operands. The register VM names them directly in one instruction.
These are actual disassembly excerpts; both programs print `42`.
[Example source](examples/bytecode.rg) · [Still image](docs/assets/bytecode.png)

</details>

## Try it

You need **CMake 3.25+, Ninja, Clang with C++20, and Python 3.9+**.
[Platform setup, Docker and installation instructions](docs/install.md).

```sh
git clone https://github.com/aadityad12/rung.git
cd rung
cmake --preset debug && cmake --build --preset debug
./build/debug/rung examples/fib.rg                       # 6765
python3 scripts/demo.py --rung build/debug/rung           # checked, guided tour
ctest --preset debug                                    # full correctness suite
```

On ARM64, enable the JIT for the complete tour:

```sh
cmake --preset release-goto-nanbox && cmake --build --preset release-goto-nanbox
python3 scripts/demo.py --rung build/release-goto-nanbox/rung
```

The tour checks engine agreement, shows bytecode, and verifies JIT compilation and fallback.
Unsupported JIT demos are explicitly skipped. `rung --help` explains usage; `rung --version`
lists the engines in that build. [Run individual demo sections or regenerate the clips](docs/demo.md).

## The engineering

![Source goes through a shared frontend to the tree-walker or bytecode VMs; the register VM feeds the ARM64 JIT](docs/assets/architecture.svg)

- **A shared runtime:** wrapping int32 arithmetic, floats, interned strings, arrays, closures
  and a precise mark-sweep collector. All engines share the language's semantics and errors.
- **A measurable optimization ladder:** bytecode, computed-goto dispatch, NaN-boxing,
  register instructions, superinstructions, global inline caches and constant folding.
- **A handwritten ARM64 JIT:** instruction encodings checked against LLVM assembler output,
  type guards, array access, and fallback to the exact bytecode instruction that failed.
- **A background compiler:** release/acquire publication, instruction-cache synchronization,
  and GC roots that keep queued functions alive. Tested under ThreadSanitizer.
- **Correctness across configurations:** conformance programs, unit tests, forced GC,
  ASan/UBSan, frontend fuzzing, and CI on macOS ARM64, Linux ARM64 and Linux x86-64.

The interpreter foundations follow *Crafting Interpreters* and Lua 5.0. The optimization
experiments, handwritten JIT, background compilation and measurement pipeline extend that
foundation. [Architecture and design decisions](docs/architecture.md) · [Testing](docs/testing.md)

## Results

![Stack, register and JIT configuration speedups across six benchmarks; outlined JIT bars mean VM fallback](docs/assets/performance.svg)

The JIT compiles functions in **one of the six main benchmarks**. Outlined bars show VM
fallback, not native-code performance. The ladder includes every regression, including
NaN-boxing slowing all six benchmarks. Results come from one M1 Pro, with some comparisons
across different measurement sessions.

**Read the investigations:** [why register bytecode dispatches fewer instructions](docs/performance.md#one-rung-up-close-stack-vm-to-register-vm),
[what surprised me](docs/performance.md#what-surprised-me), and
[methodology and limitations](docs/performance.md).

<details>
<summary>Generated numerical summary</summary>

<!-- summary:start -->
_Generated by `scripts/ladder.py` from `results/`, measured on Apple M1 Pro, macOS 26.6.2; median process total divided by calls._ `02_stack` Stack VM (switch dispatch, tagged values): 1.30× (`strcat`) to 4.99× (`sieve`) the speed of `01_tree` Tree-walker. `09_jit` + baseline JIT: 1.35× (`strcat`) to 22.64× (`loop_sum`) the speed of `01_tree` Tree-walker. The largest loss from a single rung is `04_nanbox` on `sieve`: 0.63× the speed of the row before it.
<!-- summary:end -->

</details>

<details>
<summary>Full benchmark tables, regressions and provenance</summary>

<!-- ladder:start -->
_This section is generated by `scripts/ladder.py` from the files in `results/`. Do not edit it by hand: `ladder.py --check` runs in CI and fails if it differs._

**Speedup over `01_tree`** (median process total divided by calls per process; higher is faster):

| Configuration | fib | loop_sum | sieve | nbody | strcat | closures | warmup |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `01_tree` Tree-walker | 1.00× | 1.00× | 1.00× | 1.00× | 1.00× | 1.00× | n/a |
| `02_stack` Stack VM (switch dispatch, tagged values) | 3.80× | 3.80× | 4.99× | 4.70× | 1.30× | 4.04× | n/a |
| `03_goto` + computed-goto dispatch | 4.53× | 5.22× | 9.04× | 6.17× | 1.33× | 4.66× | n/a |
| `04_nanbox` + NaN-boxed values | 4.06× | 3.89× | 5.72× | 4.80× | 1.31× | 4.25× | n/a |
| `05_register` Register VM | 5.71× | 6.82× | 13.51× | 6.67× | 1.35× | 5.11× | n/a |
| `06_super` + superinstructions | 5.76× | 10.22× | 16.81× | 6.95× | 1.35× | 5.34× | n/a |
| `07_ic` + inline caching for globals | 8.41× | 7.03× † | 16.84× | 6.96× | 1.35× | 5.21× | n/a |
| `08_fold` + constant folding / dead-code elimination | 8.36× | 7.02× | 16.82× | 6.97× | 1.34× | 5.28× | n/a |
| `09_jit` + baseline JIT | 8.33× ‡ | 22.64× | 16.53× ‡ | 6.88× ‡ | 1.35× ‡ | 5.25× ‡ | n/a |

**Marginal speedup over the previous row:**

| Configuration | fib | loop_sum | sieve | nbody | strcat | closures | warmup |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `01_tree` Tree-walker | — | — | — | — | — | — | — |
| `02_stack` Stack VM (switch dispatch, tagged values) | 3.80× | 3.80× | 4.99× | 4.70× | 1.30× | 4.04× | n/a |
| `03_goto` + computed-goto dispatch | 1.19× | 1.37× | 1.81× | 1.31× | 1.03× | 1.15× | n/a |
| `04_nanbox` + NaN-boxed values | **0.90× ▼** | **0.74× ▼** | **0.63× ▼** | **0.78× ▼** | **0.98× ▼** | **0.91× ▼** | n/a |
| `05_register` Register VM | 1.41× | 1.75× | 2.36× | 1.39× | 1.03× | 1.20× | n/a |
| `06_super` + superinstructions | 1.01× | 1.50× | 1.24× | 1.04× | 1.00× | 1.04× | n/a |
| `07_ic` + inline caching for globals | 1.46× | **0.69× ▼** † | 1.00× | 1.00× | **1.00× ▼** | **0.98× ▼** | n/a |
| `08_fold` + constant folding / dead-code elimination | **0.99× ▼** | **1.00× ▼** † | **1.00× ▼** | 1.00× | **1.00× ▼** | 1.01× | n/a |
| `09_jit` + baseline JIT | **1.00× ▼** ‡ | 3.22× | **0.98× ▼** ‡ | **0.99× ▼** ‡ | 1.01× ‡ | **1.00× ▼** ‡ | n/a |

**p99 time of one `run()` call** (nearest-rank, over every measured call of every run pooled; lower is better):

| Configuration | fib | loop_sum | sieve | nbody | strcat | closures | warmup |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `01_tree` Tree-walker | 115.47 ms | 310.22 ms | 284.47 ms | 230.46 ms | 186.70 ms | 243.56 ms | n/a |
| `02_stack` Stack VM (switch dispatch, tagged values) | 30.80 ms | 79.23 ms | 56.66 ms | 49.41 ms | 144.62 ms | 61.05 ms | n/a |
| `03_goto` + computed-goto dispatch | 26.18 ms | 57.85 ms | 31.24 ms | 37.48 ms | 140.90 ms | 53.28 ms | n/a |
| `04_nanbox` + NaN-boxed values | 29.53 ms | 77.56 ms | 49.22 ms | 48.75 ms | 143.42 ms | 57.60 ms | n/a |
| `05_register` Register VM | 21.09 ms | 46.15 ms | 20.81 ms | 35.41 ms | 139.62 ms | 48.07 ms | n/a |
| `06_super` + superinstructions | 21.19 ms | 32.06 ms | 16.75 ms | 33.77 ms | 139.31 ms | 46.24 ms | n/a |
| `07_ic` + inline caching for globals | 13.82 ms | 43.36 ms † | 17.05 ms | 34.07 ms | 139.81 ms | 49.12 ms | n/a |
| `08_fold` + constant folding / dead-code elimination | 14.06 ms | 43.32 ms | 16.79 ms | 33.76 ms | 139.32 ms | 47.77 ms | n/a |
| `09_jit` + baseline JIT | 14.25 ms ‡ | 37.84 ms | 17.26 ms ‡ | 33.66 ms ‡ | 138.60 ms ‡ | 48.04 ms ‡ | 0.82 ms |

**JIT compile time per process** (median over the runs of the total time the JIT spent deciding on and compiling functions in one process, rejected ones included; in parentheses, its share of the median total time of the timed calls):

| Configuration | fib | loop_sum | sieve | nbody | strcat | closures | warmup |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `09_jit` + baseline JIT | 2.5 µs (0.00091%) ‡ | 16.4 µs (0.0062%) | 2.9 µs (0.00088%) ‡ | 5.2 µs (0.00078%) ‡ | 2.6 µs (0.000094%) ‡ | 6.3 µs (0.00069%) ‡ | 519.3 µs (17%) |

**Background compilation** (notes D8): `jit_sync` (the JIT compiling on the engine's thread, row 09's arguments) against `jit_background` (the same with `--jit-background`). These are not ladder rows. Time of one `run()` call, p50 / p99 / max in ms, nearest-rank over every measured call of every run pooled; lower is better. Measured in one `bench.py` invocation, so interleaved (notes D5) and comparable call for call.

| Benchmark | `jit_sync` p50 | `jit_sync` p99 | `jit_sync` max | `jit_background` p50 | `jit_background` p99 | `jit_background` max |
| --- | --- | --- | --- | --- | --- | --- |
| `fib` ‡ | 13.72 | 14.09 | 15.23 | 13.74 | 14.11 | 16.91 |
| `loop_sum` | 12.26 | 31.71 | 39.07 | 12.24 | 43.15 | 43.21 |
| `sieve` ‡ | 16.65 | 17.18 | 17.28 | 16.64 | 17.11 | 17.16 |
| `nbody` ‡ | 33.29 | 33.90 | 34.34 | 33.24 | 33.64 | 33.70 |
| `strcat` ‡ | 136.47 | 138.60 | 147.10 | 136.56 | 143.03 | 159.57 |
| `closures` ‡ | 45.90 | 47.39 | 47.74 | 45.91 | 47.49 | 47.59 |
| `warmup` | 0.09 | 0.82 | 0.83 | 0.09 | 0.32 | 0.35 |

**Provenance** (from `meta` in each results file):

| Configuration | Commit | Date | Machine | Power | Measured |
| --- | --- | --- | --- | --- | --- |
| `01_tree` Tree-walker | `e618db54a8` | 2026-10-07 | Apple M1 Pro, macOS 26.6.2 | AC power | 10 runs of 20 calls |
| `02_stack` Stack VM (switch dispatch, tagged values) | `e618db54a8` | 2026-10-07 | Apple M1 Pro, macOS 26.6.2 | AC power | 10 runs of 20 calls |
| `03_goto` + computed-goto dispatch | `e618db54a8` | 2026-10-07 | Apple M1 Pro, macOS 26.6.2 | AC power | 10 runs of 20 calls |
| `04_nanbox` + NaN-boxed values | `e618db54a8` | 2026-10-07 | Apple M1 Pro, macOS 26.6.2 | AC power | 10 runs of 20 calls |
| `05_register` Register VM | `e618db54a8` | 2026-10-07 | Apple M1 Pro, macOS 26.6.2 | AC power | 10 runs of 20 calls |
| `06_super` + superinstructions | `e618db54a8` | 2026-10-07 | Apple M1 Pro, macOS 26.6.2 | AC power | 10 runs of 20 calls |
| `07_ic` + inline caching for globals | `0d05a706c5` | 2026-10-07 | Apple M1 Pro, macOS 26.6.2 | AC power | 10 runs of 20 calls |
| `08_fold` + constant folding / dead-code elimination | `0d05a706c5` | 2026-10-07 | Apple M1 Pro, macOS 26.6.2 | AC power | 10 runs of 20 calls |
| `09_jit` + baseline JIT | `12d2c10368` | 2026-10-08 | Apple M1 Pro, macOS 26.6.2 | AC power | 10 runs of 20 calls |
| `jit_sync` the JIT compiling on the engine's thread, row 09's arguments | `12d2c10368` | 2026-10-08 | Apple M1 Pro, macOS 26.6.2 | AC power | 10 runs of 20 calls |
| `jit_background` the same with `--jit-background` | `12d2c10368` | 2026-10-08 | Apple M1 Pro, macOS 26.6.2 | AC power | 10 runs of 20 calls |

▼ marks a configuration slower than the one it is compared with. † marks a cell where the spread (IQR) of the per-run totals exceeded 5% of their median in at least one of the two runs compared, so the figure is uncertain and the row should be re-measured.

‡ marks a JIT row's cell for a benchmark in which the JIT compiles no function at its default threshold, so the row measures the register VM plus the JIT's hotness counting, not machine code (notes D7, D16; `bench/jit_not_compiled.json`, checked against `rung --jit-log` by a ctest). Why: `fib`: `fib` is rejected (it calls itself through the global `fib`), and `run` is called only 20 times, below the threshold; `sieve`: `run` is rejected (it calls the global `array`); `nbody`: `root` is rejected (a float constant), `advance` and `run` (globals); `strcat`: `run` is rejected (a string constant); `closures`: `run` is rejected (a global), `make_adder` (it creates a closure), `add` and `next` (captured variables).
<!-- ladder:end -->

</details>

Tables and the chart are generated from committed `results/*.json`; CI checks both for drift.
[Reproduce the numbers](docs/performance.md#reproducing-the-numbers).

## Scope and next questions

The language is deliberately small: first-class functions and closures, fixed-size arrays,
strings, numbers, conditionals and loops. [Language rules](docs/architecture.md#the-language).

The JIT supports integer operations, control flow and array access. Calls, globals, floats,
strings and closures stay in the VM; there is no on-stack replacement. Both JIT modes require
ARM64. The three interpreters also run on Linux x86-64. The NaN-boxing regression and
`loop_sum` timing groups are still unexplained. [Full limitations](docs/performance.md#limitations-and-what-id-do-next).

## Acknowledgements

- Robert Nystrom, [*Crafting Interpreters*](https://craftinginterpreters.com/). The tree-walker
  follows `jlox`: environments as garbage-collected hash maps, and variable uses resolved to a
  number of hops beforehand. The stack VM and its compiler follow `clox` closely: local slots,
  scope tracking, upvalue resolution and jump patching in the compiler; the dispatch loop, calls,
  returns and closures in the VM; and the disassembler's layout. Anything that looks like `clox`
  probably is. The parser's precedence climbing is the same idea as `clox`'s Pratt parser,
  written as a table and a loop. The NaN-boxed value follows the book's optimization chapter too,
  with an int32 tag added.
- Roberto Ierusalimschy, Luiz Henrique de Figueiredo and Waldemar Celes,
  [*The Implementation of Lua 5.0*](https://www.lua.org/doc/jucs05.pdf) (2005). The register VM
  is based on it: locals in fixed registers with temporaries above them, the calling convention,
  one register file with each call frame a window into it, and building long array literals in
  batches the way Lua's `SETLIST` does.
- The JIT uses the standard ARM64 calling convention (AAPCS64). Its instruction encoder is
  hand-written; the LLVM assembler is used only to generate the table its tests check against,
  and is not a dependency.
- Everything past the register VM, the JIT and background compilation, goes beyond both.
- [doctest](https://github.com/doctest/doctest), vendored in `third_party/`, runs the unit tests.

## License

MIT. See [LICENSE](LICENSE). The vendored doctest is also MIT.
