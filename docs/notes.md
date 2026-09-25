# Rung: design notes and decisions

This is the running record of every design decision, the rules all engines must follow, and
(later) what each ladder rung was expected to do versus what it measured.

**Precedence.** This file is the source of truth for design decisions. Where it differs from the
original design spec (a private planning document, cited below as "spec §N"), this file wins,
and the entry says what it replaced.

**Status tags.** `DECIDED` means settled. `PROPOSED` means it's a recommendation that
still needs sign-off. Nothing `PROPOSED` should be built on without checking first.

---

## 1. Decisions log

### D1. Integers are 32-bit signed, with wraparound. `DECIDED` (2026-09-16)

- Replaces "64-bit integers" in spec §3.1.
- Why: NaN-boxing (rung 3b) packs every value into 8 bytes and has only ~48 spare bits, so a
  64-bit integer cannot fit. 32 bits fits easily. JavaScriptCore does the same (int32 inside a
  NaN-boxed value), which gives the README a precedent to cite.
- Side benefit: ARM64 `W` registers are 32-bit, so JIT'd arithmetic wraps exactly like the
  interpreters with no extra work.
- Knock-on: integer literals must fit in 32 bits (compile error otherwise). Benchmarks must be
  sized so wraparound either doesn't happen or is intentional (e.g. `loop_sum` summing to 10^8
  overflows 32 bits; either sum modulo a constant or accept the wrapped result, since it is
  still deterministic).
- Also resolves open decision 2 in spec §12 (overflow behaviour): wraparound.

### D2. Add a minimal array type. `DECIDED` (2026-09-20)

- Why: the language has no arrays, but `sieve` needs one and `nbody` effectively needs one.
  Spec §3.2 treats the feature list as fixed; this is a deliberate, one-time exception.
- Proposed surface, kept as small as possible:
  - Literal: `[1, 2, 3]`, `[]`
  - Sized constructor: `array(n, fill)` (e.g. `array(1000000, true)`)
  - Read / write: `a[i]`, `a[i] = v`
  - Length: `len(a)` (native function, like `clock()`)
  - Fixed size after creation. No push, pop, slicing, or nesting syntax beyond arrays holding
    arrays as ordinary values.
- Semantics: heap object, shared by reference (like Python lists), equality by identity,
  GC must trace elements. Index must be an int in `[0, len)`; anything else is a runtime error.
- JIT: arrays are **not** in the initial JIT whitelist. Adding array get/set to the JIT (so
  `sieve` can be JIT'd) is a stretch goal, decided once the JIT works.
- `strcat` benchmark: drop "and hash" (no string indexing exists). It still measures what it is
  for: allocation and GC pressure from building many strings.

### D3. The JIT compiles register-VM bytecode, not stack-VM bytecode. `DECIDED` (2026-09-20)

- Replaces the "same bytecode" arrow in spec §4's diagram.
- Why: a register instruction says `c = a + b`, where `a`, `b`, `c` are numbered slots in the
  function's frame. That maps straight to "load, add, store" in ARM64. A stack instruction
  sequence (`push a; push b; add; store c`) would force the JIT to simulate the stack.
- Why it makes bail-out easy: JIT'd code reads and writes the same frame slots the register VM
  uses. When a type guard fails, nothing needs translating. The VM simply resumes at the
  matching bytecode instruction with the frame exactly as the JIT left it.
- Knock-on 1: the JIT depends on the register VM. If the register VM slips, the JIT
  slips. This matches the project's "cut from the bottom" rule.
- Knock-on 2: the JIT only runs in the top ladder configuration (register VM + computed goto +
  NaN-boxing). Its type guards check NaN-box tag bits.
- Every JIT'd instruction records which bytecode instruction it came from, so a bail-out knows
  where to resume.

### D4. The ladder is measured cumulatively, and shared code is built as swappable pieces. `DECIDED` (2026-09-20)

- "Every ladder configuration" (spec §8) means these 9 configurations, each one the
  previous row plus exactly one change. It does **not** mean all 64 on/off combinations.

  | # | Configuration |
  |---|---|
  | 1 | Tree-walker |
  | 2 | Stack VM (switch dispatch, 16-byte tagged values) |
  | 3 | + computed goto |
  | 4 | + NaN-boxing |
  | 5 | Register VM (keeps computed goto + NaN-boxing) |
  | 6 | + superinstructions |
  | 7 | + inline caching for globals |
  | 8 | + constant folding / dead code elimination |
  | 9 | + baseline JIT |

- Optional extra, cheap once automated: "leave one out" from config 8 (turn each rung off
  individually from the best configuration) to show interactions between rungs.
- To avoid writing each improvement twice (once per VM):
  - **Values** go through one small interface (`is_int`, `as_int`, `make_int`, ...). Two
    implementations exist, tagged struct and NaN-boxed, selected by a build flag. Both VMs and
    the GC only ever use the interface.
  - **Dispatch** goes through a few macros (`DISPATCH()`, `CASE(op)`, ...) that expand to
    either a `switch` or computed `goto` depending on a build flag. Both VM loops use them.
- Build-time switches (value representation, dispatch style) produce separate binaries, one
  CMake preset each. Runtime switches (engine, superinstructions, inline caches, folding, JIT)
  are CLI flags.

### D5. Benchmarking and hardware counters. `DECIDED` (2026-09-20)

- **Where benchmarks run:** only on the development machine (Apple M1 Pro, 6 performance cores +
  2 efficiency cores). Never in CI, never in Docker. CI checks correctness only.
- **`perf stat` in Docker won't work.** `perf` is Linux-only, and Docker on a Mac runs Linux
  inside a virtual machine that very likely doesn't expose the CPU's hardware counters. The
  same applies to GitHub's CI machines. The spec's exit criterion "perf stat branch-miss
  comparison captured in Docker" is replaced by:
  1. **Our own counters:** a debug-build flag that counts instructions dispatched, calls, and
     allocations inside the VM. Exact and identical on every platform. This is also the number
     spec §5.5 wants for the register VM ("instruction count should drop sharply").
  2. **Apple's CPU counters (optional):** Instruments' CPU Counters template via `xctrace`.
     Requires full Xcode. Only the Command Line Tools are installed right now. Try it when
     the benchmark harness exists. If it gives branch misses, use it; if not, drop it.
  3. **Real Linux hardware (optional):** only if a lab machine or similar is easily
     available. Not required.
- **Per-iteration timing (p99):** timing happens outside the hot code. A normal
  outer loop calls the benchmark's work function N times and records the time around each
  call. The hot function itself never calls `clock()`, because a native call inside it would
  make the JIT refuse to compile it.
- **Noise control:**
  - Set the benchmark thread's QoS to `QOS_CLASS_USER_INTERACTIVE` so macOS strongly prefers
    performance cores. macOS has no way to pin a thread to specific cores (Linux's `taskset`
    has no equivalent).
  - Plugged in, Low Power Mode off, other apps closed, machine not used during a run.
  - At least 10 runs per config, first discarded, report median and IQR.
  - **Interleave** configurations (A B C A B C, not A A A B B B) so a burst of background
    activity hits every configuration equally instead of skewing one.
  - `bench.py` records CPU model, macOS version, power source, and compiler version into
    every results JSON, and flags any config whose IQR exceeds a threshold (e.g. 5% of median)
    for a rerun.

### D6. CI runs on three machines. `DECIDED` (2026-09-20)

| Runner | Engines tested | Why |
|---|---|---|
| macOS ARM64 (`macos-15`) | all, including JIT | Main platform: `MAP_JIT` path |
| Linux ARM64 (`ubuntu-24.04-arm`) | all, including JIT | JIT's plain-Linux `mmap` path. Free for public repos. |
| Linux x86-64 (`ubuntu-24.04`), inside the project `Dockerfile` | 1-3 only, JIT disabled | Portability proof. An x86 CPU cannot run ARM64 machine code. |

- The conformance runner must know which engines exist on the current platform and skip the
  JIT cleanly on x86-64 (reported as "skipped", never silently).
- Locally, Docker on the Mac gives **ARM64** Linux by default. `--platform linux/amd64` gives
  x86-64 but under emulation: fine for a correctness check, slow, and useless for timing.
- Sanitizer jobs: ASan+UBSan is one build and ThreadSanitizer is a separate build (they can't
  be combined).

### D7. `fib` is not guaranteed to be JIT'd. `DECIDED` (2026-09-20)

- The spec says "JIT covers `fib`", but under the whitelist it can't: `fib` calls itself
  through a global variable lookup plus a function call, and both are excluded.
- Plan: the JIT row shows `fib` as "not JIT'd", labelled honestly. If the JIT lands with time to spare, add a
  special case for **direct self-recursion**: the JIT emits a direct call to the function's own
  machine code, guarded by a cheap check that the global still points to the same function
  (bail out if it was reassigned).
- `loop_sum` is the one benchmark the JIT is guaranteed to cover.

### D8. Background compilation (Engine 5): measure before promising. `DECIDED` (2026-09-20)

- Risk: a JIT this small may compile a function in well under a millisecond (a guess, not a
  measurement). If the pause is too small to see, "background compilation removes the p99
  spike" cannot be shown, and by the honesty rules it must not be claimed.
- Order of work:
  1. When the JIT first works: log how long each synchronous compile takes, and which iteration it landed in.
  2. Only then build the background thread, and design the measurement so a real pause is
     visible: short iterations, per-iteration timing, and a plainly labelled warm-up benchmark
     with many distinct hot functions (a normal and legitimate JIT warm-up test).
  3. If there is still no measurable tail effect, report exactly that. The concurrency
     engineering (thread-safe handoff, TSan-clean CI) is still reported; a latency win
     is not claimed.
- Thread-safety rules, chosen so ThreadSanitizer can check everything that matters:
  - Bytecode is immutable once compiled, so the compiler thread can read it without locks.
  - The compiler thread hands back finished machine code by storing a pointer in a
    `std::atomic`. The main thread reads that atomic **in C++** before jumping into machine
    code. JIT'd machine code never reads or writes anything the other thread touches.
    (TSan only watches memory access in code compiled by clang; it cannot see inside machine
    code we generate at runtime.)
  - The GC must not free a function while it is queued or being compiled (it is kept as a GC
    root until the handoff completes).
  - On macOS, `pthread_jit_write_protect_np` is per-thread, so the compiler thread can hold
    its pages writable while the main thread keeps executing other, already-finished code.
  - Test early that TSan builds work at all alongside `MAP_JIT` on macOS.
- **TSan and `MAP_JIT` check (result, recorded when `ExecBuffer` landed).** ThreadSanitizer
  and `MAP_JIT` work together on macOS arm64 (macOS 26.6, Apple clang 21): the `tsan` preset
  builds `src/jit/exec_memory.cpp`, maps the buffers with `MAP_JIT`, and runs code out of them
  with no reports and no crashes. The `exec memory` unit tests include a two-thread case: one
  thread rewrites its own buffer (its `pthread_jit_write_protect_np` toggle flips) while the
  main thread keeps calling code from a different, finished buffer, and a case where one thread
  writes code and another runs it after a `join`. No workaround (such as excluding the JIT from
  the TSan build) was needed. The `tsan` CI job runs the whole unit-test suite on both macOS
  and Linux arm64. The expected division of labour is unchanged: TSan checks the C++ side of the
  handoff (the atomic pointer, the queue) but cannot see inside generated code.
  - Linux runners need `vm.mmap_rnd_bits` lowered to 28 before TSan starts; that is a
    kernel/TSan startup issue unrelated to the JIT and is set in `ci.yml`.

### D9. Lexical rules. `DECIDED` (2026-09-20)

- Keywords: `and else false fn for if let nil or print return true while`. `array`, `len`
  and `clock` are native functions, not keywords.
- Logical not is `!` (spec §3.1 mentions `not`, but its grammar uses `!`; one spelling only).
- Comments: `//` to end of line. No block comments.
- Identifiers: ASCII letter or `_`, then letters, digits, `_`.
- Integers: decimal digits only. The lexer accepts values up to 2147483648. That one value is
  only legal directly after unary minus (`-2147483648`), which the parser enforces, so the
  smallest int can be written as a literal. Anything larger is a compile error.
- Floats: digits `.` digits (`1.5`, `0.25`). No exponent, no `.5`, no `1.`.
- Strings: double quotes, may span lines. Escapes `\n \t \" \\` only; any other escape is a
  compile error.
- A lexer error stops at the first problem: `[line N] compile error: <message>`, exit 65.
- CLI exit codes follow `sysexits.h`: 64 bad usage, 65 compile error, 66 can't read input,
  70 runtime error.

### D10. One shared runtime for every engine. `DECIDED` (2026-09-20)

- `src/runtime/` holds everything about values that is not engine-specific: the `Value` type,
  heap objects, the garbage collector, string interning, and an **operations module**
  implementing every rule in §2 (arithmetic, comparison, equality, truthiness, printing, and
  the exact runtime error messages). Engines call these; they never reimplement them. That
  makes engines agree by construction. The only exception is the JIT, which inlines integer
  fast paths and calls the same helpers for everything else.
- **Integer math never uses signed overflow** (undefined behaviour in C++). Wrapping ops compute
  in `uint32_t` and convert back. UBSan in CI enforces this.
- **`Value`** (`src/runtime/value.h`): tagged struct today (`nil`, `bool`, `int32_t`,
  `double`, `Obj*`), behind the D4 interface (`is_int`, `as_int`, `make_int`, ...). NaN-boxing
  later swaps the implementation, not the interface.
- **Objects:** every heap object starts with a header `{ObjKind kind; bool marked; Obj* next;}`
  and lives on one intrusive list owned by `Heap`. Kinds: `String` (immutable, interned, hash
  cached), `Array`, `Native`, `TreeFunction` + `Environment` (tree-walker only), `Function`
  (bytecode prototype), `Closure`, `Upvalue` (VMs only).
- **GC:** precise, stop-the-world mark-sweep. Checked on every allocation: collect when
  `bytes_allocated > next_gc`; afterwards `next_gc = max(1 MiB, 2 × live bytes)`.
  - Roots: each engine registers a root-marking callback with the heap (its stack, frames,
    globals, environments). Values held only in C++ local variables are protected with an RAII
    `TempRoot` guard that pushes onto a heap-owned temp-root stack.
  - The intern table is weak: sweeping removes unmarked strings from it before freeing them.
  - `--gc-stress` collects on **every** allocation. CI runs the conformance suite with it, which
    is how missing roots get caught.
  - `--stats` prints to stderr: objects allocated, bytes allocated, collections, peak heap bytes.
- **Runtime errors:** `RuntimeError{line, message}`, formatted in `diagnostic.{h,cpp}` next to
  `CompileError` as `[line N] runtime error: <message>`.

### D11. A shared resolver pass after parsing. `DECIDED` (2026-09-20)

- Runs once, before any engine, and does two jobs.
- **Static errors**, reported as compile errors identically for every engine:
  - `return` outside a function: `can't return from top-level code`
  - redeclaring a local in the same scope: `variable 'x' is already declared in this scope`
    (globals may be redeclared)
  - reading a local in its own initializer (`let a = a;` inside a block):
    `can't read local variable 'a' in its own initializer`
  - more than 255 parameters, arguments, locals in one function, or captured variables in one
    function: `too many parameters` / `too many arguments` / `too many local variables` /
    `too many captured variables`. The bytecode uses one-byte operands for these, and every
    engine must reject exactly the same programs.
- **Binding annotations:** every variable use is marked either global, or local at N scopes up.
  The tree-walker follows those hops through its chained hash maps. It stays deliberately
  unoptimised (still a hash lookup per access), but it is now correct.
- Why this is needed: with plain dynamic lookup through the environment chain, this program
  prints `global` then `block` in a tree-walker but `global` twice in a VM, so the engines
  would disagree:
  ```
  let a = "global";
  { fn show() { print a; } show(); let a = "block"; show(); }
  ```
- No engine may have a limit the others don't have (e.g. bytecode jump offsets must be wide
  enough for any function, not 16 bits).

### D12. Command line and host interface. `DECIDED` (2026-09-20)

```
rung [--engine=tree|stack|register|jit] [--gc-stress] [--stats]
     [--dump-tokens | --dump-ast | --dump-bytecode]
     [--bench=N --bench-out=FILE] file.rg
```
- Default engine is `tree` until faster engines exist; revisit at the end.
- Every engine implements one C++ interface (`src/engine.h`): run a program, and call a global
  zero-argument function by name from C++ and get its result.
- **Bench mode** (`--bench=N`): run the program's top level (defines functions, prints
  nothing), then call the global function `run` N times, timing each call in C++ with
  `std::chrono::steady_clock`. Write JSON to `--bench-out`:
  `{"engine": ..., "iterations_ns": [...], "result": "<printed form of the last return value>"}`.
  `bench.py` checks that `result` is identical across engines. On macOS, bench mode sets the
  thread QoS to `QOS_CLASS_USER_INTERACTIVE` (D5).
- Engines run on a dedicated thread with a 512 MiB stack, so the tree-walker (which uses C++
  recursion) reaches the 10,000-frame limit without crashing, even under ASan.

### D13. Conformance test format. `DECIDED` (2026-09-20)

- Replaces spec §8's paired `.expected` files: expectations sit in comments next to the code.
- Tests live in `tests/conformance/<topic>/<name>.rg`.
  - `// expect: TEXT` is one line of stdout. All `expect:` lines, in file order, must equal
    stdout exactly.
  - `// expect runtime error: MSG`: stderr's first line must be
    `[line L] runtime error: MSG`, where L is the line the comment is on, and the exit code 70.
  - `// expect compile error: MSG`: same shape, `compile error`, exit code 65.
  - No annotation of either error kind means exit code 0 and empty stderr.
- `tests/run_conformance.py --rung PATH --engine E [--gc-stress] [FILTER]` runs every test,
  prints a per-failure diff, and exits non-zero on any failure. CMake registers it as a ctest
  test once per engine that exists.

### D14. Register bytecode: 64-bit instruction words. `DECIDED` (2026-09-25)

The register VM (ladder rung 3c) and the JIT (D3) run this format. Its definition is in
`src/bytecode/register_code.h`; the compiler is `src/compiler_reg.cpp`. The instruction
words and the calling convention below were specified by the owner's issue; everything under
*Details chosen while implementing* is the implementer's choice and is open to review.

- **One instruction is one fixed 64-bit word:** `op` 8 bits, `A` 16, `B` 16, `C` 16, and 8
  spare bits (used for flags, below). Jumps put a signed 32-bit offset in `B:C` (called `sBx`;
  `B` is the low half), and instructions that name a constant or global by index use the same 32
  bits (`Bx`).
- **Why 16-bit operands, where Lua 5.0 has 32-bit words and 8-bit registers (255 registers):**
  the resolver limits a function to 255 locals, parameters, and call arguments (D11) and the
  nesting limit is 200 (§2.6), so a register count above 255 is reachable by ordinary programs
  (a call at each of 200 nesting levels with 255 arguments needs about 51,000). With 8-bit
  registers the compiler would have to reject some program the stack VM and the tree-walker
  accept, which D11 forbids. Sixteen bits reach 65,535. The cost is a word twice as wide as
  Lua's, so code is bigger and the instruction cache holds fewer of them. This is a trade made
  for a uniform, easily decoded format (every field at a fixed bit position; no variable-length
  or extra-argument words), and its cost is not yet measured. The frame size is computed per
  function and checked: a function needing more than 65,535 registers is the compile error
  `function too large`, which the resolver's limits and the nesting limit keep out of reach.
- **Constants as operands ("RK").** Instructions whose `B` or `C` is a value (arithmetic,
  comparison, `NEG`, `NOT`, index get/set, `PRINT`, `RETURN`) take either a register or a
  constant. The flag bits in the spare byte say which (`kFlagBConst`, `kFlagCConst`), so a
  constant index gets all 16 bits of its field. A constant whose index does not exceed 65,535
  is an operand; any other is first loaded with `LOADK` (32-bit `Bx`) into a temporary. Chosen
  over separate opcodes per operand shape because it keeps the opcode list short; each operand
  fetch in the VM then tests a flag, which a later rung (superinstructions, D4 row 6) can
  specialise. `nil`, `true`, `false`, ints, floats, and strings are all constants, and the
  compiler stores each distinct one once per function.
- **Calling convention (Lua 5.0).** Callee in register `A`, its arguments in `A+1 ... A+B`, the
  result written back to register `A`. The callee's frame starts at register `A+1`, so its
  parameters are its registers `0 ... arity-1` (there is no slot for the function itself as in
  the stack VM). A function's `frame_size` is one more than the highest register it names,
  including the callee and arguments of its own calls.
- **Locals and temporaries.** A local variable lives in a fixed register for its whole scope: a
  local's register is its position among the function's live locals, so leaving a scope frees
  its registers without any instruction. Temporaries are allocated above the locals in stack
  order and freed (the free pointer restored) when the expression that needed them ends.
- **Instructions:** `MOVE LOADK LOADNIL LOADTRUE LOADFALSE GET_GLOBAL SET_GLOBAL DEFINE_GLOBAL
  GET_UPVALUE SET_UPVALUE ADD SUB MUL DIV MOD EQ NE LT LE GT GE NEG NOT JUMP JUMP_IF_FALSE
  JUMP_IF_TRUE CALL CLOSURE CAPTURE CLOSE RETURN RETURN_NIL PRINT ARRAY ARRAY_APPEND INDEX_GET
  INDEX_SET`. Globals are still looked up by name (`Bx` is the constant index of the interned
  name), so inline caching (rung 3e) still has something to remove. Comparisons produce a bool
  in a register; there is no fused compare-and-jump yet (that is a superinstruction).

**Details chosen while implementing.**
- There is one `JUMP` for both directions (a loop is a negative offset), and offsets count
  instructions from the one after the jump. `JUMP_IF_FALSE` / `JUMP_IF_TRUE` do not change their
  register, so `and` / `or` keep the deciding operand's value (§2.2).
- `CLOSURE A Bx` is followed by one `CAPTURE` word per upvalue (`A` = 1 for a register of the
  enclosing frame, 0 for one of its upvalues; `B` = the index). They are operands of `CLOSURE`:
  the VM reads them while creating the closure and skips them. `CLOSE A` closes every open
  upvalue at or above register `A`; the compiler emits it at the end of a block that declared a
  captured local (from the lowest captured one), and the VM's `RETURN` closes everything.
- Array literals are built by `ARRAY A B C` (R[A] = the C values R[B..B+C-1], C at most 50) and,
  for longer literals, further `ARRAY_APPEND A B C` batches. So an array of any length needs
  only about 51 registers, where a single instruction naming every element would need one per
  element (the stack VM's `ARRAY` count is 24 bits).
- **Assignments are expressions in Rung, which Lua's are not**, so two hazards exist that Lua
  does not have, and the compiler handles both: (1) reading a local in place as an operand
  while a later operand of the same instruction assigns it (`a + (a = 5)`, or a call that
  assigns a captured `a`): the earlier operand is copied first whenever a later one contains a
  call or an assignment to a local. (2) building a value directly in a variable's register
  when that expression still reads the variable (`a = (b + 1) and a`): a temporary is used and
  moved once. **The VM must read all operands of an instruction before writing its result**
  (`ADD a, a, 1` is legal), and, for `CALL`, must copy the result into `A` only after the call
  finishes.
- The register bytecode lives in `ObjFunction::reg` (a `RegChunk`); the stack VM's bytecode in
  `ObjFunction::chunk`. A function has one or the other, and the GC marks both constant pools.

---

## 2. Semantics contract: rules every engine must follow exactly

The tree-walker, both VMs, and the JIT must produce identical stdout, identical error message,
and identical exit code for every program. Each rule below gets at least one conformance test.
`DECIDED` (2026-09-20).

### 2.1 Numbers

| Case | Rule |
|---|---|
| int `+ - *`, unary `-` | 32-bit two's-complement wraparound |
| int literal outside 32-bit range | compile error |
| int `/` int | integer result, truncated toward zero (C behaviour; matches ARM64 `SDIV`) |
| int `/` 0 and int `%` 0 | runtime error `division by zero`. **The JIT must check explicitly: ARM64 `SDIV` silently returns 0.** |
| `-2147483648 / -1` | wraps to `-2147483648` (also what ARM64 `SDIV` returns) |
| `-2147483648 % -1` | `0` |
| `%` sign | follows the left operand (C behaviour), e.g. `-7 % 3 == -1`. ARM64 has no remainder instruction; JIT uses `SDIV` then `MSUB`. |
| `%` with a float operand | runtime error (ints only) |
| int op float (either side) | int converts to float, result is float |
| float `/` 0.0 | IEEE result (`inf`, `-inf`, `nan`), no error |
| int `==` float | numeric comparison: `1 == 1.0` is `true` |
| `<` `>` `<=` `>=` | numbers only (int/float mixing allowed); anything else is a runtime error |

### 2.2 Other types

| Case | Rule |
|---|---|
| truthiness | only `false` and `nil` are falsy; `0`, `0.0`, `""`, `[]` are truthy |
| `and` / `or` | short-circuit; return the deciding operand's value, not a forced boolean |
| string `+` string | concatenation |
| string `+` non-string | runtime error (no implicit conversion) |
| string `==` | by content (cheap because all strings are interned) |
| function / array `==` | by identity |
| `let x;` | `x` is `nil` |

### 2.3 Printing (must be byte-identical across engines)

| Value | Output |
|---|---|
| `nil`, `true`, `false` | `nil`, `true`, `false` |
| int | decimal, e.g. `-42` |
| float | see the float algorithm below (`3.0`, `0.1`, `1e+20`, `-0.0`, `nan`, `inf`, `-inf`) |
| string | raw contents, no quotes |
| function | `<fn name>`; native functions `<native fn>` |
| array | `[1, 2.5, hi]`: elements printed by these same rules, `, ` separated, `[]` when empty. An array that contains itself (directly or indirectly) prints the inner occurrence as `[...]` |

**Float algorithm** (one implementation in `src/runtime/`, so every engine and platform matches):
1. Any NaN prints `nan` (never `-nan`). `+inf` prints `inf`, `-inf` prints `-inf`.
2. Otherwise, for precision p = 1, 2, ..., 17: format with `snprintf("%.*g", p, x)`, parse
   back with `strtod`; stop at the first p that gives back exactly `x`.
3. If the result contains none of `.`, `e`, append `.0`. So `3.0`, `-0.0`, `0.1`, `1e+20`,
   `1.5e-07`.

(`std::to_chars` was rejected: its shortest form differs in format from `%g`, and older Apple
libc++ gates it by OS version.)

### 2.4 Errors and exits

| Case | Rule |
|---|---|
| compile error | stderr `[line N] compile error: <message>`, stop at first error, exit code `65` |
| runtime error | stderr `[line N] runtime error: <message>`, exit code `70` |
| success | exit code `0` |
| undefined variable (read or assign) | runtime error `undefined variable 'x'` |
| calling a non-function | runtime error `can only call functions` |
| wrong argument count | runtime error `expected A arguments but got B` |
| array index not an int / out of range | runtime error `array index must be an int` / `array index out of range` |
| call depth | hard limit of **10,000** frames in every engine, runtime error `stack overflow`. The tree-walker uses host recursion, so it must count depth itself and fail at the same limit. |

- Conformance compares stdout exactly, exit code exactly, and the first line of stderr exactly.
- A JIT bail-out before a runtime error must still report the correct line number (the JIT's
  instruction-to-bytecode map from D3 provides it).
- Output must never depend on when the GC runs.
- Conformance tests never print `clock()` results.

### 2.5 Runtime error messages (complete list)

Every runtime error any engine can raise, with its exact message. Adding a new runtime error
means adding it here first.

| Situation | Message |
|---|---|
| `+` with operands that aren't two numbers or two strings | `operands must be two numbers or two strings` |
| `- * /` or `< <= > >=` with a non-number operand | `operands must be numbers` |
| `%` with a non-int operand | `operands of '%' must be ints` |
| unary `-` on a non-number | `operand must be a number` |
| int `/` or `%` by zero | `division by zero` |
| reading or assigning an undefined global | `undefined variable 'NAME'` |
| calling something that isn't a function | `can only call functions` |
| wrong number of arguments (Rung or native function) | `expected A arguments but got B` |
| indexing (`a[i]` or `a[i] = v`) something that isn't an array | `can only index arrays` |
| array index not an int | `array index must be an int` |
| array index `< 0` or `>= len` | `array index out of range` |
| `array(n, fill)` with `n` not an int or negative | `array size must be a non-negative int` |
| `len(x)` with `x` not an array or string | `len expects an array or a string` |
| the 10,001st nested call | `stack overflow` |

- `len` works on strings too (length in bytes); `strcat` uses it.
- The line reported is the line of the operator, call's `(`, or index's `[` that failed.

### 2.6 Other rules every engine must share

- A `for` loop's variable is one variable shared by all iterations (as in C and Lox), because
  `for` is desugared to `while` in the parser. Closures created in the body all see its final
  value.
- **Nesting limit:** expressions and statements may nest at most **200** deep (parentheses,
  operators, calls, blocks, `if`/`while` bodies all count). Deeper is the compile error
  `nesting too deep`. This keeps every recursive C++ pass (parser, resolver, tree-walker,
  compilers) safe from native stack overflow, and bounds the register VM's register count.
- Compile error messages are listed in §2.7 by the parser issue; conformance tests pin each one.

### 2.7 Compile error messages

_(One row per message, with a test for each. Parser messages are below; the resolver adds its
own rows. Lexer messages are described in D9 and tested in `tests/unit/lexer_test.cpp`.)_

The reported line is the line of the offending token (the `Eof` token's line at end of input),
except `invalid assignment target`, which reports the line of the `=`.

| Situation | Message |
|---|---|
| a token that cannot start an expression | `expected expression` |
| missing `)` closing a parenthesised expression | `expected ')' after expression` |
| missing `)` closing a call's arguments | `expected ')' after arguments` |
| missing `]` closing an index | `expected ']' after index` |
| missing `]` closing an array literal | `expected ']' after array elements` |
| expression statement without `;` | `expected ';' after expression` |
| `print` without `;` | `expected ';' after value` |
| `let` without `;` | `expected ';' after variable declaration` |
| `return` without `;` | `expected ';' after return value` |
| `let` not followed by a name | `expected variable name after 'let'` |
| `fn` not followed by a name | `expected function name after 'fn'` |
| function name not followed by `(` | `expected '(' after function name` |
| a parameter that is not a name (including a trailing comma) | `expected parameter name` |
| parameter list without `)` | `expected ')' after parameters` |
| function header not followed by `{` | `expected '{' before function body` |
| block without `}` | `expected '}' after block` |
| `if` / `while` / `for` not followed by `(` | `expected '(' after 'if'` / `'while'` / `'for'` |
| `if` / `while` condition without `)` | `expected ')' after condition` |
| `for` without `;` after its condition | `expected ';' after loop condition` |
| `for` without `)` after its clauses | `expected ')' after for clauses` |
| `=` after something that is not a variable or an index | `invalid assignment target` |
| `2147483648` anywhere except directly after unary `-` | `integer literal '2147483648' is too large for a 32-bit int` |
| expressions or statements nested more than 200 deep | `nesting too deep` |

Notes on the nesting count (§2.6): a level is added by `(`, `[` in an array literal, a call's
argument list, an index, a unary operator, the right side of `=`, a `{ }` block, and the body of
an `if` / `while` / `for`. A top-level statement or expression is level zero, and long flat
chains such as `1 + 1 + ... + 1` do not nest.

**Resolver messages** (notes D11). The resolver runs after the parser succeeds and stops at the
first error. Lines are the offending token's line, with these specifics: a name error (`let`,
function name, parameter) is the line of the name itself; `too many arguments` is the line of the
call's `(`; `too many captured variables` is the line of the variable use that pushed a function
over the limit; `can't return from top-level code` is the line of `return`.

| Situation | Message |
|---|---|
| `return` outside any function | `can't return from top-level code` |
| a name declared twice in the same local scope (`let`, `fn`, or parameter) | `variable 'x' is already declared in this scope` |
| a local read or assigned inside its own `let` initializer (`{ let a = a; }`) | `can't read local variable 'a' in its own initializer` |
| more than 255 parameters | `too many parameters` |
| more than 255 arguments in one call | `too many arguments` |
| more than 255 locals live at once in one function | `too many local variables` |
| more than 255 distinct variables captured by one function | `too many captured variables` |

Rules behind the table, which every engine relies on:

- Globals (top-level `let` / `fn` outside any block) are never checked for redeclaration and are
  never counted against any limit.
- A function's parameters and the statements directly in its body share **one** scope, so
  `fn f(a) { let a; }` is a redeclaration and a use of `a` in the body is `a@0`. A nested block
  adds a scope.
- Every function and every block is one scope, so the "hops" of a local use is the number of
  those scopes between the use and the declaration, counted across function boundaries.
- "Locals" counts parameters, `let`s, and the names of functions declared inside blocks or
  functions. A block's locals stop being live when it ends. The top-level script is treated as a
  function for this limit (its locals are the ones inside blocks, including `for` loop
  variables).
- A variable is "captured" by a function when the function, or any function nested inside it,
  uses a local of a function that encloses it. It is counted once per function however often it
  is used, and it counts in every function between the use and the owner (clox counts upvalues
  the same way). Globals are never captured.
- A local being initialised counts as declared: in `{ let a = 1; { let a = a; } }` the inner
  read is the error, not a read of the outer `a`. Assigning to such a local (`let a = (a = 1);`)
  is the same error.

---

## 3. Resolved from spec §12

| # | Spec open decision | Resolution |
|---|---|---|
| 1 | Value representation before NaN-boxing | Tagged struct (spec recommendation). Behind the value interface in D4. `DECIDED` |
| 2 | Integer overflow | 32-bit wraparound (D1). `DECIDED` |
| 3 | String interning | Intern all strings (spec recommendation). `DECIDED` |
| 4 | Benchmark sizes | Tree-walker takes roughly 2-10 s per benchmark; sized when the benchmarks are written, respecting 32-bit ints. Open |
| 5 | Name | Rung, `.rg` files. Binary is `rung`, never `rg` (that's ripgrep). `DECIDED` |

---

## 4. Open questions

_(none right now)_

---

## 5. Per-rung notes (fill in as work happens)

For each rung: what was expected, what was measured, why they differed. For the JIT: every
crash and its cause.

### JIT crashes and their causes

- **Intermittent SEGV calling freshly written code, Linux arm64, asan preset only (about 5% of
  runs).** First suspected the instruction cache. It was not: the same code with no flush at all
  never failed in isolation, and the fault was in the calling C++, not in the generated code.
  Cause: UBSan's function check (`-fsanitize=function`, part of the asan preset) reads the 8
  bytes *before* the target of every C++ call through a function pointer, looking for a type
  signature. With the code at offset 0 of a fresh `mmap`, that read hit the last 8 bytes of the
  previous page, which is unmapped whenever the kernel places the mapping next to a gap. Fix:
  `ExecBuffer` puts 16 zero bytes in front of the code (zeros mean "no signature", so the check
  passes), so every caller of `entry()` is protected. Found by looping the test 300 to 600
  times in CI; a single run passed most of the time.
