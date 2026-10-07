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

### D15. NaN-boxed Value layout. `DECIDED` (2026-10-06, issue #15)

- `RUNG_NANBOX` (CMake option, default OFF; presets `release-goto-nanbox`, `asan-goto-nanbox`)
  swaps the 16-byte tagged struct in `src/runtime/value.h` for one `uint64_t`. Only that header
  reads the flag (D4). `static_assert(sizeof(Value) == 8)` in this mode.
- The layout, top 16 bits first (`s` = sign, `e` = 11 exponent bits, `q` = quiet bit 51,
  `t` = bit 50, then bits 49..48):

  ```
  63 62      52 51 50 49 48 47                                0
   s eeeeeeeeeee q  t  .  .  ------------- payload --------------
  double  any non-NaN bit pattern, stored as itself
  NaN     0x7FF8'0000'0000'0000   every NaN is folded to this one (t = 0)
  int32   0x7FFD'0000'iiii'iiii   low 32 bits = the int
  nil     0x7FFE'0000'0000'0000
  false   0x7FFF'0000'0000'0000
  true    0x7FFF'0000'0000'0001
  Obj*    0xFFFC'pppp'pppp'pppp   sign set, pointer in the low 48 bits
  ```
- A pattern with bits 50..62 all set (`0x7FFC` prefix, or `0xFFFC` with the sign) is "not a
  double": `is_float` is one AND and compare. No real double has that prefix, because every NaN
  is canonicalised in `make_float` to `0x7FF8...`, which has bit 50 clear. Without this, a NaN
  produced with a payload (an operand's payload propagating, or x86's default `0xFFF8...`)
  could equal a tag and be read back as `nil`, an int or a pointer.
- Bit 50 is used as well as the quiet bit so the canonical NaN (`0x7FF8...`, the one ARM64 and
  `std::numeric_limits<double>::quiet_NaN()` produce) is never inside the tag space (clox does
  the same, for Intel's "QNaN floating-point indefinite").
- `is_int` checks the whole high word (`bits & 0xFFFF'FFFF'0000'0000 == 0x7FFD'0000'0000'0000`),
  so bits 32..47 of an int are always zero and `make_int` writes the int through `uint32_t`
  (D1). `is_bool` is `(bits | 1) == true`.
- Pointers: user-space addresses on arm64 macOS and Linux (and x86-64 Linux) use at most 48
  bits. `Heap::link` checks `pointer_fits_in_value` for every object it allocates, in every
  build type, and aborts with a message if one does not fit; the check is one compare next to a
  `new`.
- The default-constructed `Value` stays trivial (no initialiser), like the tagged struct, so the
  stack VM's uninitialised value stack keeps costing nothing until it is touched.
- Equality, truthiness and printing are unchanged and still live in `src/runtime/ops.cpp`:
  `nan != nan` and `-0.0 == 0.0` come from comparing the doubles, never the bits.

### D16. Baseline JIT: hotness, whitelist, calling convention, bail-out. `DECIDED` (2026-10-07, issue #23)

`--engine=jit` is the register VM with a method JIT attached: hot functions are compiled to
ARM64 machine code, and everything else stays in the VM. The hotness rule, the whitelist, the
registers-in-memory design, the guards and the bail-out-by-index protocol were specified by the
owner's issue; everything under *Details chosen while implementing* is the implementer's choice
and is open to review. Code: `src/jit/jit_compiler.{h,cpp}` (whitelist and code generation),
`src/jit/jit.{h,cpp}` (executable memory, log, statistics), the hooks in `src/vm_reg.cpp`.

- **Other rungs.** `--engine=jit` accepts `--fold` (a front-end pass) and `--inline-cache` (the
  register VM's global cache, which the JIT never touches: functions that use globals are not
  compiled). The suite also runs as `conformance-jit-fold` and `conformance-jit-inline-cache`.
- **Where it exists.** Only in builds with an arm64 CPU *and* `RUNG_NANBOX` (CMake defines
  `RUNG_JIT`): the code is ARM64 and its type guards test NaN-box tags (D3, D15). Presets
  `release-goto-nanbox` (the one measured) and `asan-goto-nanbox`. Elsewhere `--engine=jit` is the
  usage error `engine 'jit' is not available in this build`, exit 64, and the ctest entries
  `conformance-jit` / `conformance-jit-gc-stress` are reported as *Skipped* (the runner checks
  that rung refuses the engine, lists every test as skipped and exits 77; D6).
- **Hotness.** Each `ObjFunction` counts its calls plus its backward `JUMP`s (loop back-edges).
  When the count reaches `--jit-threshold=N` (default 1000) the function is compiled at once,
  on the engine's thread. A function that gets hot through a call runs machine code from that
  same call (its frame is at instruction 0, so nothing needs translating); one that gets hot
  through a back-edge runs machine code from its next call, because entering a running loop
  (on-stack replacement) is not built. A function is tried once: compiled or rejected, never
  retried. The top-level script is never compiled: it runs once, so without on-stack replacement
  its code could never be entered.
- **Whitelist.** A function compiles only if every instruction is one of: `MOVE`, `LOADK` of an
  int, nil or bool constant, `LOADNIL/LOADTRUE/LOADFALSE`, `ADD SUB MUL DIV MOD NEG`,
  `EQ NE LT LE GT GE` (operands: registers or int constants), `JUMP JUMP_IF_FALSE JUMP_IF_TRUE`,
  `RETURN` (of a register or an int, nil or bool constant) and `RETURN_NIL`, and its frame has at
  most 4096 registers. Globals, upvalues, closures, calls, `print`, arrays, `!`, string or float
  constants: the function is rejected and logged, and stays in the VM. `fib` is rejected (it
  reads the global `fib`, D7); `loop_sum` compiles.
- **Code shape.** Registers stay in memory (D3): every bytecode instruction becomes "load the
  operands from `[base + r*8]`, check them, compute, box, store to `[base + a*8]`". Nothing is
  kept in machine registers across instructions, so at every instruction boundary the frame
  holds exactly what the VM would have.
- **Type guard.** An int's NaN box has `0x7FFD0000` in its high 32 bits (D15). A guard is
  `lsr x11, xN, #32; cmp w11, w2; b.ne exit`, with `w2 = 0x7FFD0000` set up on entry. The int is
  the low 32 bits, so there is nothing to unbox; `W`-register arithmetic wraps exactly like Rung
  ints (D1), and a 32-bit result already has a zero high half, so boxing is one `orr` with
  `x1 = 0x7FFD'0000'0000'0000`. Comparisons box with `cset` + `orr` of `false`'s bits. Conditional
  jumps need no guard: a value is falsy only if it equals the bits of `false` or of `nil`.
- **Division and modulo** (§2.1). `cbz` on the divisor exits before `sdiv`, because ARM64
  `SDIV` returns 0 for a zero divisor. A constant divisor is known at compile time: no check
  when it is not zero, an unconditional exit when it is. `%` is `sdiv` then `msub`.
  `INT32_MIN / -1` and `INT32_MIN % -1` need nothing: `sdiv` gives `INT32_MIN`, then `msub`
  gives 0.
- **Calling convention** (AAPCS64; Apple's arm64 ABI is the same for these registers). The VM
  calls the code as the C function `uint32_t f(Value* base)`:

  | Register | Use |
  |---|---|
  | `x0` | in: `base`, the frame's register 0; out: `w0` = bytecode index to resume at |
  | `x1`-`x4` | constants set up on entry: int tag, int tag's high word, `false`, `nil` |
  | `x9`-`x12` | scratch: operand B, operand C, guard / quotient, result |
  | `x16`, `x17`, `x18` | never touched (linker veneers; Apple's platform register) |
  | `x19`-`x28`, `x29`, `x30`, `sp` | never touched, so nothing is saved and `ret` uses `x30` |

  Every register used is one the caller already expects a call to clobber (`x0`-`x7` arguments,
  `x9`-`x15` temporaries), the code calls nothing and uses no stack, so there is **no prologue or
  epilogue at all**: no frame record is pushed, `sp` never moves (so its 16-byte alignment is
  untouched), and the caller's frame pointer stays valid for debuggers and profilers.
- **Bail-out protocol.** Every exit is `mov w0, #index; ret`. Each instruction reads and checks
  all its operands before it writes anything, so when a guard or a zero-divisor check fails the
  frame still holds the state from *before* that instruction. The code returns that
  instruction's index; the VM sets its pc there and executes the instruction itself, so a float
  takes the VM's float path and `1 / 0` raises `division by zero` with the VM's message and line
  (§2.4). After a bail-out the rest of that call runs in the VM. `RETURN` / `RETURN_NIL` exit the
  same way with their own index and the VM performs the return (closing upvalues, popping the
  frame), so the machine code never needs to know how frames work. The VM tells the two apart by
  the opcode at the index: a return is a normal exit, anything else is a bail-out (logged).
  Every bytecode instruction's first machine instruction is recorded (`GeneratedCode::starts`,
  the D3 map), and each exit stub carries its bytecode index, so no lookup happens at run time.
- **Memory.** One `ExecBuffer` (`src/jit/exec_memory.h`, issue #21) per compiled function, owned
  by the JIT until the engine is destroyed, even if the function object is collected first; so a
  function's `jit_entry` pointer can never dangle while the engine runs.
- **Observability.** `--jit-log` writes one stderr line per compile (function, bytecode
  instructions, machine instructions, bytes, compile time in ns), per rejection (the first
  instruction outside the whitelist and why) and per bail-out (function, instruction, opcode,
  line). `--stats` adds `jit: N functions compiled (B bytes of code), M rejected, K bail-outs,
  T ns compiling`. Bench mode's JSON gains `"jit_compile_ns"`, the total compile time of the
  whole process (top level and every iteration; from `Engine::jit_compile_ns()`, absent for the
  other engines), and `scripts/bench.py` keeps it per run and as `jit_compile_ns_median` in
  `results/<row>.json`, so the JIT row can report what compiling cost next to what it saved.
  Compile time counts the whitelist check, code generation, and mapping and writing the
  executable memory, for rejected functions too.

**Details chosen while implementing.**
- *The VM loop is a template, `execute_loop<bool kJit>`.* The JIT's two hooks (counting and
  entering machine code after a frame is pushed; counting a backward jump) exist only in
  `execute_loop<true>`, so `--engine=register` runs exactly the loop it ran before (D4: each rung
  changes one thing). The cost is a second copy of the loop in the binary.
- *The JIT state lives on `ObjFunction`* (`jit_hotness`, `jit_status`, `jit_entry`), so the check
  on every call is a load and a compare, not a table lookup. Background compilation (D8, Engine
  5) will have to make `jit_entry` the `std::atomic` handoff that D8 describes.
- *`!` is not compiled*, although it would need no guard, because the issue's whitelist does not
  list it; adding it is a few lines.
- *No give-up policy.* A compiled function whose guard fails on every call (always called with
  floats, say) enters machine code and bails out on every call. That is correct but wasteful;
  counting bail-outs and discarding the code is left for later, once a benchmark shows it.
- *Whole-instruction granularity.* Two operands that are the same register are guarded twice,
  and a comparison followed by a conditional jump stores the bool and loads it back. These are
  the obvious next optimisations for this design, not done here (a baseline JIT).

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
- **Chain limit** (resolves Q1): one expression may be at most **1000** links long. A link is
  one node that has an operand: a binary or logical operator (`+ - * / % == != < <= > >= and or`),
  a call, an index, a unary operator, an array literal, or an assignment. The chain length of an
  expression is the number of links on the longest path from its root down to a leaf, and
  parentheses are not links, so `(1 + 1) + 1` is 2 long. Longer is the compile error
  `expression chain too long`. The nesting limit above counts only syntax that nests in the
  source; `1 + 1 + ... + 1`, `f(1)(1)...(1)` and `a[0][0]...[0]` nest nothing, yet the parser
  turns each into a left-leaning tree as tall as the chain is long, and every recursive pass
  (resolver, `--dump-ast`, the AST destructor, the compilers, the tree-walker) follows that
  height. Together the two limits bound the native recursion of every such pass: at most 200
  nested statements or groups, plus one expression at most 1000 links tall. Every engine must
  accept and reject exactly the same programs (D11), so this is a language rule, not a parser
  detail.
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
| an expression more than 1000 links long (§2.6, chain limit) | `expression chain too long` |

Notes on the nesting count (§2.6): a level is added by `(`, `[` in an array literal, a call's
argument list, an index, a unary operator, the right side of `=`, a `{ }` block, and the body of
an `if` / `while` / `for`. A top-level statement or expression is level zero, and long flat
chains such as `1 + 1 + ... + 1` do not nest (they have their own limit, below).

Notes on the chain length (§2.6): the line reported for `expression chain too long` is the line of
the token that adds the link that goes over 1000: the operator, the call's `(`, the index's `[`,
the unary operator, the array literal's `[`, or the `=`. Because the length is the height of the
syntax tree, a group in the middle of a chain counts with the links after it:
`1 + (<chain of 600>) + 1 + ... + 1` with 400 links after the group is 1001 long even though it
nests once. An assignment `a = v` or `a[i] = v` is one link above its value `v`; `a[i] = v` adds
nothing above its target `a[i]` unless `v` is taller than the target.

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
| 4 | Benchmark sizes | Each benchmark is sized so that `--engine=tree --bench=20` takes roughly 2-10 s (release preset, development Mac). Constants are in the table below; the timings only guided sizing and are not results. `DECIDED` |

Benchmark sizes (`bench/*.rg`, issue #7). Each `run()` returns a checksum, kept in
`bench/expected.json`. No value wraps in any of them (D1): sums are reduced with `%`.

| Benchmark | Size | Why this size |
|---|---|---|
| `fib` | `fib(27)` | about 635,000 calls per iteration; `fib(24)` was too fast to time |
| `loop_sum` | 2,000,000 loop steps, sum reduced `% 1000003` | `i % 7` and the reduction keep the sum far inside 32 bits; 3,000,000 steps ran past the 10 s ceiling |
| `sieve` | primes up to 600,000 | 200,000 was below the 2 s floor; the array is one allocation of 600,001 slots |
| `nbody` | 5 bodies, 5,000 steps of `dt = 0.01` | 500 steps was far too fast; `sqrt` is 12 Newton iterations (Rung has no `sqrt`) |
| `strcat` | 30,000 strings of 20 bytes, then one string grown to 12,000 bytes | most `+` results are garbage immediately, which is the GC pressure; the long string is quadratic in total bytes copied |
| `closures` | 300,000 closures created and called, then 300,000 calls through one shared counter | exercises allocation of closures and upvalues, then upvalue reads and writes |
| 5 | Name | Rung, `.rg` files. Binary is `rung`, never `rg` (that's ripgrep). `DECIDED` |

---

## 4. Open questions

### Q1. Long flat chains overflow the native stack (found by the fuzzer, issue #12). `RESOLVED`

The nesting limit (§2.6) bounds *nesting*, but `1 + 1 + ... + 1`, `a and a and ... and a`,
`f(1)(1)...(1)` and `a[0][0]...[0]` do not nest: they are left-associative chains, so the
parser builds them in a loop and no depth counter moves. The resulting AST is still a left spine
as deep as the chain is long, and every recursive pass follows that spine. Feeding a chain of
about 8,000 terms (16 KB of source) to the resolver overflowed the 8 MiB main-thread stack in the
fuzz build (AddressSanitizer reported `stack-overflow` in `Resolver::expr_`); 6,000 terms passed.
The AST destructor and `dump_ast` recurse the same way, and so will the tree-walker and the
compilers.

Options were: (1) count each link toward the nesting limit; (2) a separate limit on the length of
one chain; (3) make every pass iterative along the spine. **The owner chose option 2, with a limit
of 1000**, so no pass has to be iterative and future engines are protected too. The rule and its
message are in §2.6 (chain limit) and §2.7.

Implementation note: the limit is on the *height* of an expression's syntax tree, not on the
length of one run of operators. Counting only a single run would leave a hole: nothing stops
`(1 + 1 + ... + (1 + 1 + ...) + 1 + ...) + ...` from stacking one 999-link run inside another, 200
groups deep, for a tree 200,000 tall that fits the nesting limit. Before the change a single flat
chain of 20,000 terms crashed the asan build (`AddressSanitizer:DEADLYSIGNAL` from stack
exhaustion), and so did 150 nested groups of 250 links each; after it both are the compile error
`expression chain too long`. The unit tests pin both, plus the 1000/1001 boundary for every
construct that builds a chain (`tests/unit/parser_test.cpp`, "chain limit").

The fuzz target's 4096-byte input cap existed only because of this question and is removed.

---

## 5. Per-rung notes (fill in as work happens)

For each rung: what was expected, what was measured, why they differed. For the JIT: every
crash and its cause.

### The measurement pipeline (`scripts/bench.py`, `scripts/ladder.py`)

How D5's noise control became code, and the choices D5 left open:

- **`--runs 11` means 11 rounds, the first discarded**, which gives D5's "at least 10 runs, first
  discarded": 10 measured runs per (config, benchmark). Each run is one fresh process calling
  `run()` `--iterations` (20) times; its *total* is the sum of those calls.
- **Interleaving is per round, not per config.** Every round runs all (config, benchmark) pairs
  once in an order shuffled by a recorded seed (default 1), so no pair is always first or last
  and a burst of background load is spread over all of them. Same seed, same order.
- **Statistics.** Median and IQR (Q3 - Q1, inclusive quartiles) of the 10 per-run totals decide
  the speedups and the `noisy` flag (IQR above 5% of the median, D5). p50, p99 and max are
  nearest-rank over all 200 per-call times pooled across runs, so they are always an observed
  time, never an interpolation.
- **Speedups compare per-call times** (median total divided by the iteration count), so results
  taken with different `--iterations` still compare. Cells are shown to two decimals, because
  one decimal would display a 3% loss as `1.0x`. A ratio below 1.0 is marked `▼`; a cell whose
  config, or the config it is compared with, was noisy is marked `†`.
- **The README table is generated and checked.** `ladder.py` rewrites only the text between the
  `ladder:start` and `ladder:end` markers; `ladder.py --check` (a CI job) regenerates it and
  fails on any difference, so a hand edit, or a results file changed without regenerating, fails
  the build.
- **A results file is one invocation.** Running a subset of benchmarks replaces the whole file
  for that config (the change report names anything dropped), because mixing benchmarks measured
  at different times would defeat the interleaving.
- **Dirty-tree check ignores `results/`**, since earlier invocations write there.

### Engine 1: tree-walker

- **Expected:** the slowest engine and the baseline every later number is measured against. It
  is not measured here: benchmarks are not written yet, so there is no result to report.
- **Runtime errors are returned, not thrown.** The first version threw a C++ `RuntimeError` from
  the failing node and caught it in `run()`. Under AddressSanitizer that broke exactly where it
  matters: the conformance test that overflows the stack at the 10,001st nested call unwinds a
  native stack of roughly 70 to 100 MB, and ASan refuses to clean up ("unpoison") a stack larger
  than 64 MB when an exception starts unwinding, warns `ASan is ignoring requested
  __asan_handle_no_return`, and then aborts on a false positive while unwinding. The 512 MiB
  stack (D12) is big enough to reach the depth limit, but not something ASan can unwind
  through. So an error is recorded in the engine (`error_`), each evaluation checks it after
  every sub-evaluation and returns at once, and `return` is a separate status (`Flow::Return`).
  Nothing in the engine throws. Any later engine that recurses on the C++ stack should do the
  same.
- **Scope objects are created for every block and every call, matching the resolver's scopes
  one to one** (D11): a block that declares nothing still creates one, because the resolver
  counted it when it computed `hops`. This is the cost the later engines remove.

### Ladder rung 3a: computed-goto dispatch

- **What changed.** `RUNG_COMPUTED_GOTO` (CMake option, default OFF; presets `release-goto` and
  `asan-goto`) makes the macros in `src/vm/dispatch.h` expand to a table of label addresses and a
  `goto *table[opcode]` at the end of every handler, instead of one `switch`. The loop body in
  `vm_stack.cpp` is the same text in both builds. Only `vm_stack.cpp` is compiled with
  `-Wno-gnu-label-as-value`.
- **Expected.** One shared indirect branch becomes one per handler, which gives the branch
  predictor a separate history per opcode. A real gain is plausible, but modern predictors
  (Apple's included) predict a single indirect branch well from global history, so a small gain
  is also plausible and would be a legitimate finding.
- **What the disassembly shows** (`otool -tvV` on the `release` and `release-goto` binaries,
  looking at `StackEngine::execute`, which has 36 opcodes):

  | Build | `br xN` in `execute` |
  |---|---|
  | `release` (switch) | 1 |
  | `release-goto` | 40 |

  The switch build has a single `br x9`, reached from every handler through the loop's top: it
  bounds-checks the opcode (`cmp w8, #0x23`), loads a 2-byte offset from a jump table, adds it to
  a base and branches:

  ```
  ldrb  w8, [x26], #0x1        ; fetch opcode
  cmp   w8, #0x23              ; bounds check (36 opcodes)
  b.hi  ...
  adrp/add x11 ...             ; jump table
  ldrh  w10, [x11, x8, lsl #1]
  add   x9, x9, x10, lsl #2
  br    x9                     ; the one shared indirect branch
  ```

  The goto build repeats this at the end of each handler, with no bounds check and an 8-byte
  address table:

  ```
  ldrb  w9, [x27]              ; fetch opcode
  mov   x27, x8                ; ip advanced
  adrp/add x8 ...              ; dispatch table
  ldr   x8, [x8, x9, lsl #3]
  br    x8                     ; this handler's own indirect branch
  ```

  There are 40 branches for 36 opcodes: the compiler made a few extra copies of the dispatch
  tail (which handlers got them was not traced). Either way the shared branch is gone, so the
  mechanism the rung claims is present in the generated code.
- **Measured.** Not yet. The measurement needs the benchmark harness and an exclusive machine
  (D5), so the speedup, the row in the results table and the explanation are still to be filled
  in here.
- **Handlers cannot jump out of a destructor's scope.** `goto *` may not leave the scope of a
  variable with a destructor, so handlers that use `std::string` or `std::vector` keep them in an
  inner block that closes before `RUNG_NEXT()`. The `switch` build never needed this.

### Ladder rung 3b: NaN-boxed values

- **What changed.** `RUNG_NANBOX` selects the 8-byte `Value` described in D15. Nothing outside
  `src/runtime/value.h` reads the flag; the only other source change is the 48-bit pointer check
  in `Heap::link`. Every unit test and the whole conformance suite (tree-walker and stack VM,
  with and without `--gc-stress`) pass in `asan-goto-nanbox`, which CI runs on macOS arm64 and
  Linux arm64. The register VM does not exist yet, so "both VMs" is the stack VM for now.
- **Expected.** Halving the value size halves the bytes moved for every push, pop, local
  access, constant load and array element, so more of the working set stays in L1 and each
  64-byte cache line holds 8 values instead of 4. Against that, every type check and unbox now
  costs a mask and compare (and `make_float` a NaN check) where the tagged struct read one tag
  byte. Programs dominated by float arithmetic pay the NaN check on every result; programs that
  touch large arrays should gain most.
- **Memory side.** The stack VM's value stack is 6,754,304 slots reserved up front, so its
  address-space reservation drops from 16 to 8 bytes a slot; pages are only touched as the
  stack grows, so resident memory changes only for deep recursion. Array element buffers and
  constant tables also halve. Peak RSS per benchmark comes from the results JSON once measured.
- **Measured.** Not yet. Row `04_nanbox` needs the benchmark harness (`scripts/bench.py`,
  `ladder_configs.json`), which is not in the repository yet, and an exclusive machine (D5).
  Expected versus measured, value-stack bytes and peak RSS are to be filled in here then.

### Ladder rung 3c: register VM

- **What changed.** `--engine=register` runs the register bytecode of D14 (`src/vm_reg.cpp`).
  It uses the same dispatch macros as the stack VM (`src/vm/dispatch.h` now takes the opcode
  enum and its list from the VM that includes it), the same `Value` interface, and the same
  `src/runtime/` operations, so computed goto and NaN-boxing apply to it unchanged. Every unit
  test and the whole conformance suite pass on it, with and without `--gc-stress`, in `debug`,
  `asan`, `asan-goto` and `asan-goto-nanbox`.
- **The register window** (Lua 5.0, section 7). There is one register file for the whole run.
  A frame is a window into it: register `r` of the running function is the slot `base + r`. For
  `CALL A B` the callee is in register `A` and its arguments in `A+1..A+B`, so the callee's
  window starts at `base + A + 1` and its parameters are already in its registers `0..B-1`:
  nothing is copied. `RETURN` writes the result to `base[-1]` of the returning window, which is
  the caller's register `A`. This keeps every register in memory at `base + index`, which is
  what the JIT will read and write (D3); `RegChunk::line_at` maps an instruction index to its
  source line for error reporting and, later, bail-outs.
- **Details chosen while implementing** (open to review):
  - *The register file never moves*, for the same reason as the stack VM's value stack: open
    upvalues are raw pointers to registers. It is reserved once at the stack VM's size
    (6,754,304 slots, uninitialised, so pages are touched only as frames reach them). The
    register VM knows each callee's exact window size (`frame_size`), so a call checks that the
    whole window fits and needs no slack for temporaries. Running out before the 10,000-frame
    limit takes windows that start, on average, more than 675 registers above their caller's.
  - *A call sets the callee's registers above its arguments to nil* before the frame runs (Lua
    5.0's `luaD_precall` does the same). Without it the collector would have to know which
    registers are initialised: a returned callee leaves stale values above its caller's window,
    and the collector frees what they point at once they are no longer marked, so marking a
    stale register later would read a freed object. With it, the collector marks one
    contiguous run from slot 0 to the highest end of any live window, every slot of which is
    a valid value. The cost is `frame_size - arity` stores per call.
  - *The entry callee sits in slot 0.* The script's closure (or the function `call_global`
    calls) is put in slot 0 and the entry frame's window starts at slot 1, so every frame has
    a `base[-1]` and calls need no special case.
  - *An error never unwinds C++ frames*: the loop returns `false` like the stack VM, and the
    engine closes every open upvalue and empties the frame stack.
- **Expected.** Fewer instructions dispatched for the same work, because a register instruction
  names its operands where the stack VM pushes them first: `GET_LOCAL`, `CONST`, `POP` and most
  `SET_LOCAL`s disappear. Each remaining instruction does more decoding (a 64-bit word, a flag
  test per RK operand), so time per instruction rises somewhat. Spec §5.5 expects this to be the
  largest single win of the ladder; that is to be measured, not assumed.
- **Instructions dispatched, both VMs** (exact counts from the VM counters, D5: the `debug`
  build, `rung --engine=stack|register --stats FILE`; these are counts, not timings). The
  benchmarks do not exist yet, so the programs are conformance tests:

  | Program (`tests/conformance/`) | Stack VM | Register VM | Register / stack |
  |---|---|---|---|
  | `functions/recursion_fibonacci.rg` | 264,840 | 143,460 | 0.54 |
  | `programs/sieve_of_eratosthenes.rg` | 1,632 | 798 | 0.49 |
  | `programs/bubble_sort.rg` | 895 | 372 | 0.42 |
  | `programs/gcd_and_collatz.rg` | 2,843 | 1,064 | 0.37 |
  | `programs/string_building_stress.rg` | 5,138 | 2,325 | 0.45 |
  | `programs/linked_list_in_arrays.rg` | 182 | 111 | 0.61 |
  | `closures/accumulator_generator.rg` | 44 | 34 | 0.77 |

  Calls are identical in both (22,070 for the Fibonacci test). Where the drop comes from, for
  the Fibonacci test: of the 121,380 instructions saved, 55,173 are `GET_LOCAL`, 44,136 are
  `CONST` (the register VM still dispatches 4 `LOADK`s; every other constant is an operand),
  22,070 are `POP` and 1 is `NIL`. The instructions that do the work (`LT`, `JUMP_IF_FALSE`,
  `SUB`, `ADD`, `GET_GLOBAL`, `CALL`, and the returns) are dispatched exactly as often in both.
  So the register VM removes data movement, not work: `fib` compiles to 12 instructions instead
  of 23.
  Programs dominated by arithmetic on locals (`gcd_and_collatz`) lose the most; ones dominated
  by calls and closures (`accumulator_generator`) the least, since a call still costs a
  `GET_GLOBAL` or `GET_UPVALUE`, argument moves and the `CALL` itself.
- **Measured.** Not yet. Row `05_register` (`release-goto-nanbox`, `--engine=register`) needs the
  benchmark harness (`scripts/bench.py`), which is not in the repository yet, and an exclusive
  machine (D5). The time per benchmark, the instructions dispatched per benchmark for both VMs,
  and the explanation of any difference from the expectation are to be filled in here then. If
  the register VM turns out slower, count instructions per benchmark before profiling (spec §13).

### Ladder rung 3d: superinstructions

- **What changed.** `--superinstructions` (register VM only; any other engine refuses it with exit
  code 64, so a run can never be labelled with a rung it did not use) runs a peephole pass over
  each function's finished register code (`src/superinstructions.cpp`) and fuses six pairs of
  adjacent instructions. `--stats=pairs` (both VMs; needs a build with the VM counters) prints how
  often opcode X was dispatched immediately after opcode Y. Every unit test and the whole
  conformance suite pass with the flag on, with and without `--gc-stress`
  (`conformance-register-superinstructions[-gc-stress]`), with every register-VM rung at once
  (`conformance-register-all-rungs`: fusion, inline cache and folding), and the benchmark
  checksums are checked with it (`bench-check-register-superinstructions`, release builds).
- **What a superinstruction saves.** Dispatching an instruction means fetching its word, decoding
  its opcode and doing an indirect branch to its handler. A fused pair does that once for two
  instructions. The work the two halves do is unchanged (same operand decoding, same operations
  from `src/runtime/`), so the saving is the dispatch between them and nothing else: it is
  bounded by the share of run time that dispatch takes, which is what this rung measures.
- **How a pair is fused: the second word stays where it is.** A fused instruction is the first
  instruction of the pair with only its opcode byte changed (`LT` becomes `LT_JUMP_IF_FALSE`; A, B,
  C and the flags are as the compiler wrote them). The second instruction is not removed: its
  word stays in the next slot, and the fused handler reads it with `insn = *pc++` and runs it as
  the instruction it still is. Nothing moves, so none of these needs repairing: jump offsets,
  the line table (each half keeps its own line, so an error in the second half is reported on the
  second half's line), and the inline cache (indexed by instruction position, rung 3e). A jump that
  lands on the second word runs it as an ordinary instruction, which is why it must stay a whole
  instruction. The price is that the code does not get smaller: the bytecode has the same number of
  words with and without the flag (`--stats` prints the same `bytecode:` line). The alternative,
  deleting the second word, would make code smaller but needs every jump to be retargeted, and
  every jump that lands on the deleted word to be found first.
- **The fused handlers are the plain handlers run back to back.** In `vm_reg.cpp` the body of each
  instruction that can be half of a pair is a macro (`BODY_ADD`, `BODY_JUMP_IF_FALSE`, ...), used
  by the plain handler and by the fused ones, so no rule exists twice. `LT_JUMP_IF_FALSE` is
  `BODY_LT(); insn = *pc++; BODY_JUMP_IF_FALSE();`. The comparison still writes its bool to its
  register: `and` and `or` keep the deciding value (`a < b and c` is `false` when the test fails,
  notes §2.2), and the pass cannot know whether that register is read later.
- **The pass.** One left-to-right scan per function, run when the function's code is complete. If
  the opcodes of word `i` and `i+1` are a listed pair, word `i` becomes the fused opcode and the
  scan continues at `i+2`: a pair never shares a word with the next pair, so `MOD ADD JUMP` fuses
  `MOD ADD` and leaves `JUMP` alone, although `ADD JUMP` is also listed. Running the pass twice
  changes nothing. `CAPTURE` words (operands of `CLOSURE`) are never half of a pair. The table is
  `kFusedPairs` in `bytecode/register_code.h`, which also has `fused_first` and `fused_second`, so
  the unfused program can be recovered from a fused one (and the JIT can treat a fused word as
  its two halves).
- **Where the six came from: data, from this benchmark suite.** `scripts/pair_counts.py` runs
  `rung --engine=register --stats=pairs --bench=1` on each benchmark and writes
  `docs/pair_counts.txt`, the 30 most frequent pairs of each benchmark (a ctest, `pair-counts`,
  fails if the committed file is not what the build produces). Counts of consecutive pairs,
  summed over the six benchmarks, for the pairs that matter here:

  | Pair | Dispatches | Where | Adjacent in the code? | Fused? |
  |---|---|---|---|---|
  | `ADD -> JUMP` | 5,489,361 | every loop that ends with `i = i + 1` | yes | yes (`ADD_JUMP`) |
  | `MOD -> ADD` | 4,600,000 | `loop_sum` (4.0M), `closures` | yes | yes (`MOD_ADD`) |
  | `LT -> JUMP_IF_FALSE` | 4,389,778 | five of the six | yes | yes (`LT_JUMP_IF_FALSE`) |
  | `JUMP -> LT` | 3,639,135 | the loop's jump back to its test | no: a jump | no |
  | `ADD -> MOD` | 2,600,000 | `loop_sum`, `closures` | yes, overlaps `MOD ADD` | no |
  | `JUMP_IF_FALSE -> MOD` | 2,000,000 | `loop_sum` | yes, but it is a branch | no |
  | `LE -> JUMP_IF_FALSE` | 1,850,365 | `sieve` | yes | yes (`LE_JUMP_IF_FALSE`) |
  | `INDEX_SET -> ADD` | 1,324,452 | `sieve` (1.25M), `nbody` | yes | yes (`INDEX_SET_ADD`) |
  | `DIV -> ADD` | 1,200,240 | `nbody` | yes | yes (`DIV_ADD`) |
  | `RETURN -> ADD` | 917,810 | after a call returns | no: crosses a return | no |

  A pair is only worth fusing if the two instructions are next to each other in the code, not
  only in time: `JUMP -> LT` and `CALL -> LT` are consecutive because a jump or a call went
  somewhere, and they cannot be fused. Of the pairs that can, the six with the most dispatches
  were taken. A pair that overlaps a better one gains nothing extra (only one of `MOD ADD` and
  `ADD MOD` fits in `MOD ADD MOD ADD`), so `ADD MOD` was left out. A first version of the pass
  also fused `GET_GLOBAL SUB` (635,620 dispatches, `fib` only), `MOVE MOVE` (650,000),
  `INDEX_GET INDEX_GET` (525,060, `nbody` only), `MUL MUL` (350,019), `ADD MOD` (600,000) and
  `GT JUMP_IF_FALSE` (50,010); each saved fewer dispatches than the smallest of the six kept
  (`ADD_JUMP`, 964,789), so they were removed to keep the set small.
- **Tuned to this benchmark suite (spec §5.5).** The six fusions are the pairs that happen most
  in six programs that were written for another purpose, and the suite shapes the choice.
  `loop_sum` keeps its sum in range with `%` because ints wrap (D1), so remainder-then-add is
  4.0M of `MOD_ADD`'s 4.6M dispatches, and a program with no `%` gets nothing from it. `DIV_ADD`
  is the Newton step in `nbody`'s `root`. `LE_JUMP_IF_FALSE` is `sieve`'s loop test, and `>`,
  `>=`, `==` and `!=` tests have no fused form because the suite hardly uses them (`>` is 50,010
  dispatches in `nbody`). Different programs would pick different pairs, and a gain measured on
  these six is not evidence about other programs. That is why the pairs are reported by benchmark:
  the honest reading is "this is what fusing the common pairs of this suite does to this suite".
- **Instructions dispatched, without and with the fusions** (exact counts from the VM counters,
  D5; `debug` build, `--stats --bench=1`, so the script's top level plus one call of `run()`;
  counts, not timings; both `vm:` lines are in `docs/pair_counts.txt` and
  `docs/pair_counts_fused.txt`):

  | Benchmark | Plain | Fused | Fewer | `LT_JUMP_IF_FALSE` | `LE_JUMP_IF_FALSE` | `ADD_JUMP` | `MOD_ADD` | `DIV_ADD` | `INDEX_SET_ADD` |
  |---|---|---|---|---|---|---|---|---|---|
  | `closures` | 9,600,020 | 8,400,018 | 12.5% | 600,002 | 0 | 0 | 600,000 | 0 | 0 |
  | `fib` | 4,131,542 | 3,495,921 | 15.4% | 635,621 | 0 | 0 | 0 | 0 | 0 |
  | `loop_sum` | 14,000,008 | 8,000,007 | 42.9% | 2,000,001 | 0 | 0 | 4,000,000 | 0 | 0 |
  | `nbody` | 8,491,383 | 6,395,976 | 24.7% | 790,152 | 0 | 30,015 | 0 | 1,200,240 | 75,000 |
  | `sieve` | 9,902,205 | 6,201,614 | 37.4% | 0 | 1,850,365 | 600,774 | 0 | 0 | 1,249,452 |
  | `strcat` | 1,910,017 | 1,212,015 | 36.5% | 364,002 | 0 | 334,000 | 0 | 0 | 0 |

  The last six columns are dispatches of that superinstruction. Each replaced two dispatches, so
  Plain minus Fused is their sum (a ctest checks that identity). The number of calls is unchanged.
- **Expected.** Fewer dispatches by the percentages above, and a smaller gain in time than in
  dispatches, because only the dispatch is saved and the handlers' work is the same. How much one
  dispatch costs on an M1 with computed goto (rung 3a) is the open question the rung answers: the
  branch predictor already predicts the indirect branches of a loop this regular well, so the
  saving might be small. The rung should help most where dispatch is the largest share of the run
  (`loop_sum`, `sieve`, `strcat`), and least in `fib` and `closures`, where calls and frame setup
  dominate.
- **Measured.** Not yet. Row `06_super` needs an exclusive machine (D5). Time per benchmark, and
  the explanation of any difference from the expectation, are to be filled in here then. A result
  that is small, or negative, is reported as it is.
- **The unfused loop is not exactly the old loop.** The fused handlers are in the same dispatch
  function as the plain ones. Without the flag they are never reached, but the compiler may lay
  the function out differently (more labels, more code), so the previous row can move for reasons
  unrelated to this change. Unlike the inline cache, there was no way to keep the plain loop
  identical without duplicating the loop. If row 05 is measured on this build and differs from
  an earlier measurement, say so.
- **What the ladder rows must pass.** D4 makes the ladder cumulative, so `--superinstructions`
  belongs in the arguments of every row from 06 on: `06_super` is
  `--engine=register --superinstructions`, `07_ic` is `--engine=register --superinstructions
  --inline-cache`, and `08_fold`, the only one of these in `scripts/ladder_configs.json` (with
  `--engine=register --fold`), needs `--superinstructions --inline-cache --fold` before it is
  measured, or it would measure folding without the two rungs below it. Rows 02 to 07 are not in
  the file yet and the arguments of `08_fold` were not changed here, for the reason given under
  rung 3e (adding them now would make the table compare the new rows with the tree-walker). The
  three flags compose, and the suite runs with all three at once
  (`conformance-register-all-rungs`).
- **For later rungs.** The JIT (D3) compiles register bytecode, so with this flag on it sees fused
  words: it must treat a fused word as its two halves (`fused_first`, `fused_second` and the next
  word), or be given unfused code. No global access is fused, so rung 3e's rule (the cache slot at
  an instruction's own index belongs to that instruction) is not exercised by a fused word; if one
  is added, its first half must run before `pc` moves, as `GET_GLOBAL`'s does now.

### Ladder rung 3e: inline caching for globals

- **What changed.** `--inline-cache` (register VM only; any other engine refuses it with exit
  code 64, so a run can never be labelled with a rung it did not use) makes `GET_GLOBAL` and
  `SET_GLOBAL` skip the hash lookup after their first execution. Every unit test and the whole
  conformance suite pass with the flag on, with and without `--gc-stress`
  (`conformance-register-inline-cache[-gc-stress]`), and the benchmark checksums are checked with
  it too (`bench-check-register-inline-cache`, release builds).
- **How it works.**
  - *A global's cell.* The global table is `std::unordered_map<ObjString*, Value>`. The C++
    standard guarantees that growing the table (a rehash) moves its buckets but never its
    elements, so the address of a global's `Value` is a stable cell for as long as the engine
    lives. Nothing in Rung can remove a global, so the cell is never freed either. The table
    itself did not change, which is why the VM without the flag is untouched.
  - *The cache.* Each function has `ObjFunction::global_cache`, one `Value*` slot per
    instruction of its register code (the issue's "side table indexed by instruction position";
    slot `i` belongs to instruction `i`). The engine sizes it, empty, for the script and every
    function nested in it before anything runs. The first time a `GET_GLOBAL` / `SET_GLOBAL`
    executes, its slot is empty: it looks the name up, stores the cell's address, and uses it.
    Every later execution of that instruction reads the slot and goes straight to the cell.
  - *Why nothing is ever invalidated.* A cache usually has to be thrown away when what it
    remembers changes. Here what is remembered is *where* the variable lives, not its value, and
    neither the cell's address nor the fact that it is defined can change. `let x = 2;` on an
    existing global and `x = 3;` both write through the same cell, so a function that cached
    `x` earlier reads the new value. A native replaced by `fn len(...)` is the same cell with a
    new value. This is the whole reason the table has stable cells.
  - *Undefined globals.* A lookup that finds nothing stores nothing and raises `undefined
    variable 'x'` at the instruction's line, exactly as without the cache; if `x` is defined
    later, the same instruction finds it the next time it runs. So the issue's "a cell that
    exists but was never defined must be distinguishable from a defined one" holds by a
    different design: a name has a cell only once it is defined, so a cell that exists is
    always a defined one and no flag is needed. (A design that created cells ahead of the
    definition would need the flag; this one does not create them.)
  - *The loop has two instantiations* (`execute_loop<false>` and `<true>`), chosen once per
    `execute()`. Without the flag the dispatch loop is the plain lookup it was before this rung,
    with no test of the flag anywhere in it and no extra work when a frame is entered or left, so
    the previous row of the ladder is not made slower to flatter this one. With the flag,
    entering or leaving a frame loads two more pointers (the function's code start and its cache).
- **Cost.** One 8-byte slot per instruction of every function, even though only global accesses
  use one: the cache is as large as the bytecode itself (an instruction is 8 bytes). Slots
  sit in a separate array from the code, so the instruction stream itself is unchanged. A
  dense numbering of only the global-access sites would be smaller but needs an operand the
  64-bit instruction word does not have spare (D14). The cost has not been measured.
- **Expected.** A hit replaces hashing a pointer, a bucket walk and a key comparison with a load
  of the slot and a branch, so the gain is proportional to how much of a benchmark is global
  accesses. Instruction counts from the VM counters (debug build, `--inline-cache --stats
  --bench=1`: the script's top level plus one call of `run()`; these are counts, not timings):

  | Benchmark | Instructions dispatched | `GET_GLOBAL` | Cache hits | Cache misses |
  |---|---|---|---|---|
  | `fib` | 4,131,542 | 635,621 | 635,618 | 3 |
  | `closures` | 9,600,020 | 300,001 | 299,999 | 2 |
  | `nbody` | 8,491,383 | 60,013 | 60,006 | 7 |
  | `strcat` | 1,910,017 | 30,001 | 29,999 | 2 |
  | `sieve` | 9,902,205 | 1 | 0 | 1 |
  | `loop_sum` | 14,000,008 | 0 | 0 | 0 |

  Every global access after the first per instruction is a hit (the misses are the distinct
  instructions that ran). `fib` has the most, one for every call (it calls itself through the
  global `fib`, D7), and about one instruction in 6.5 is a `GET_GLOBAL`, so it is the benchmark
  to look at; `loop_sum` and `sieve` do their work in locals, so the rung cannot change them and
  a measured difference there would be noise. The saving per hit is small against a whole `CALL`
  and its frame setup, so a large gain on `fib` would be surprising; what it is, is to be measured.
- **Measured.** Not yet. Row `07_ic` (the previous row plus `--inline-cache`) needs an exclusive
  machine (D5). Time per benchmark and the explanation of any difference from the expectation
  are to be filled in here then.
- **What the ladder rows must pass.** D4 makes the ladder cumulative, so `--inline-cache` belongs
  in the arguments of every row from 07 on. `scripts/ladder_configs.json` has no `07_ic` row yet
  (rows 02 to 07 are added as they are measured), but it does have `08_fold`, whose arguments
  are `--engine=register --fold`: that row must gain `--inline-cache` (and the superinstruction
  flag, rung 3d) before it is measured, or it would measure folding without the rung below it.
  The measured rows were not touched in this change. `--fold` and `--inline-cache` compose, and
  the conformance suite runs with both at once (`conformance-register-fold-inline-cache`).
- **For later rungs.** An instruction that fuses a global access (a superinstruction, rung 3d)
  must keep the rule "the slot at this instruction's own index is this instruction's", and the
  JIT can embed the cell address a cache slot holds instead of looking the name up (D7's
  self-recursion check is exactly "does the global's cell still hold this function").

### Ladder rung 3f: constant folding and dead-code elimination

- **What changed.** `--fold` runs `fold()` (`src/fold.cpp`) on the resolved syntax tree before
  any engine sees it, so it is not specific to one VM: the tree-walker and both compilers get
  the same simplified program, and `--dump-ast --fold` and `--dump-bytecode --fold` show it. The
  pass is measured on the register VM (D4 row 8). Without `--fold` nothing in it runs. The whole
  conformance suite passes with `--fold` on all three engines (`conformance-*-fold`, plus the
  register VM with `--gc-stress`), and `tests/conformance/folding/` pins the edge cases.
- **What it folds.** A unary or binary operation whose operands are literals becomes a literal,
  computed by the operations in `src/runtime/ops.cpp` (D10), never by a second copy of the
  rules. `2147483647 + 1` becomes `-2147483648`, `-7 % 3` becomes `-1`, `1 / 2.0` becomes `0.5`,
  `1 / 0.0` becomes `inf`, `1 == 1.0` becomes `true`. `!` of a literal folds. `lit and b` and
  `lit or b` become `b` or `lit` according to the literal's truthiness (the deciding operand's
  value, §2.2); the operand that is not chosen is never evaluated, with or without folding.
  Dead code: the statements after a `return` in the same block, the branch of an `if` that a
  literal condition rules out, and a `while` whose literal condition is falsy. A statement is
  only ever removed or replaced by another that was already in the same position, never
  moved into another block, so no variable's scope distance (the resolver's "hops", D11) changes.
- **What it refuses to fold.**
  - *Anything that would raise a runtime error.* The pass asks the ops whether the operation
    succeeds and leaves the node alone if it does not: `1 / 0`, `1 % 0`, `5 % 2.0`, `1 + true`,
    `1 < nil`, `-"a"`. The error then happens at run time, on the operator's line, after the
    output that precedes it (§2.4), exactly as without `--fold`. Folding it would either move
    the error to compile time (a program that prints before failing would print nothing) or lose
    it.
  - *String operations.* `"a" + "b"` would allocate a string while the program is being
    prepared, outside any engine's GC roots, and constant string arithmetic is rare. They are
    left alone, as are `==` and `!=` on strings. (A string literal does take part as the test of `!`, `and` and `or`, where
    only its truthiness matters.)
  - *`while (true)`* and any other truthy constant condition: that is an infinite loop and has
    to stay one. The condition is not removed from a loop that stays.
  - *Code after an `if` that may not return.* Only a `return` statement ends its block; a
    `return` inside an `if` that is not constant ends nothing.
- **Details chosen while implementing** (open to review):
  - The pass is one recursive walk, bottom-up, so `1 + 2 * 3` folds in one visit and a
    condition is folded before the `if` or `while` it controls is judged. Its recursion depth
    is bounded by the same two limits as the resolver's (§2.6).
  - When a statement disappears where the grammar needs one (the body of an `if` or `while`),
    it becomes an empty block. In a block's statement list it is simply removed.
  - A string literal's truthiness is asked of the ops (`is_truthy`) through a Value interned
    in a private heap inside the pass, rather than restated in the pass.
  - `-0.0` is a literal now (`-` applied to `0.0`), so the compilers' constant pools have to
    keep it apart from `0.0`. The register compiler already did (it keys floats by bit
    pattern; `negative_zero_survives_folding` and the same-function test pin it) and the stack
    compiler never merges constants.
  - `--dump-ast` printed `inf` and `nan` as `inf.0` and `nan.0`, which could not happen before:
    only a folded float can be one. It prints `inf`, `-inf` and `nan` now.
  - `--stats` reports `fold: N expressions folded, M statements removed` (with `--fold`) and
    `bytecode: ... instructions, ... code bytes, ... constants` for the stack and register
    engines. The bytecode line is computed by compiling the program a second time on a scratch
    heap, so a run without `--stats` does no extra work.
  - The front-end fuzz target now also folds and dumps the folded tree, since folding is a pure
    tree rewrite that needs no execution.
- **Expected.** Small, as the issue says. A compile-time pass can only remove work that is
  constant, and a hot loop's arithmetic is on variables. Where a program does contain a constant
  expression, folding removes the instructions that evaluate it each time the statement runs, so
  a hot loop gains only if its body contains one. Dead code removal matters even less, because
  nobody writes it on purpose; it exists for the generated and edited code a larger program
  would have.
- **Bytecode size, before and after `--fold`** (`rung --engine=register|stack --stats FILE`, the
  `bytecode:` line; these are counts, not timings). The programs are the six benchmarks
  (`bench/*.rg`). Instructions are register words, or whole stack instructions:

  | Benchmark | Register instructions | Stack instructions | Expressions folded |
  |---|---|---|---|
  | `closures` | 55 → 55 | 88 → 88 | 0 |
  | `fib` | 22 → 22 | 35 → 35 | 0 |
  | `loop_sum` | 14 → 14 | 31 → 31 | 0 |
  | `nbody` | 241 → 237 | 531 → 521 | 10 |
  | `sieve` | 35 → 35 | 94 → 94 | 0 |
  | `strcat` | 35 → 35 | 73 → 73 | 0 |

  Five of the six have nothing to fold. `nbody`'s ten are the negative literals in its body table
  (`-1.16...` is a negation applied to `1.16...` in the source, a `NEG` at run time, a literal
  after folding). They are in `make_bodies`, which `run` calls once before its 5,000 calls to
  `advance`, so none of them is in a hot loop. No measurable change in the timed work is
  expected for any of the six, and a row that shows one is noise or an effect to be explained,
  not a result of the pass.
- **Measured.** Not yet. Row `08_fold` (`release-goto-nanbox`, `--engine=register --fold`) is
  in `scripts/ladder_configs.json`, but measuring it needs an exclusive machine (D5), so there is
  no `results/08_fold.json` yet. The times, and the explanation of any difference from the
  above, are to be filled in here then. The row's arguments are only what this rung adds; D4
  makes the ladder cumulative, so they gain the flags of rungs 3d and 3e (superinstructions,
  inline caching) when those land and before 08 is measured.

### Engine 4: baseline JIT

- **What changed.** `--engine=jit` (D16): the register VM compiles hot whitelisted functions to
  ARM64 machine code. Every unit test and the whole conformance suite pass on it with
  `--jit-threshold=1` (every eligible function compiled on its first call), with and without
  `--gc-stress`, in `asan-goto-nanbox` and `release-goto-nanbox`, which CI runs on macOS arm64
  and Linux arm64. `tests/conformance/jit/` adds the cases the JIT is most likely to get wrong:
  a guard failing in the middle of a compiled loop, ints then floats and strings through the same
  compiled function, division and modulo by zero (a register divisor and a constant one), every
  wraparound rule, comparisons at the int extremes, truthiness of non-bool values, nested loops.
- **What compiles.** Run over the conformance suite with `--jit-threshold=1 --jit-log`, most
  functions are rejected, as the whitelist intends: the commonest reasons are a captured
  variable, a global (every call of a global function, so all recursion, D7), creating a
  closure, and a string constant. The ones that compile are small int functions (`gcd`,
  `collatz_steps`, loops that return early, arithmetic helpers) and `loop_sum`.
- **The VM counters do not see machine code.** `--stats` on `--engine=jit` counts only the
  instructions the VM dispatched, so a compiled loop's instructions disappear from the `vm:`
  line; the `jit:` line above it says how many functions were compiled and how often they bailed
  out.
- **Expected.** For `loop_sum`, each bytecode instruction becomes a handful of machine
  instructions with no dispatch, no operand decoding and no RK flag tests, so a large gain over
  the register VM is plausible; the loads and stores to the frame on every instruction (D3) and
  the guards are what is left. `fib` is not compiled (D7) and should be unchanged except for
  the counting on each call. Nothing is measured yet.
- **Measured.** Not yet. Row `09_jit` (`release-goto-nanbox`,
  `--engine=jit --inline-cache --fold`) is in `scripts/ladder_configs.json`, but measuring it
  needs an exclusive machine (D5), so there is no `results/09_jit.json` yet; the measurement and
  the per-function compile times are issue #24. The row's arguments are cumulative (D4): the JIT
  is the register VM plus machine code, so it accepts the register VM's `--inline-cache`, and the
  row gains the superinstruction flag (rung 3d) when that lands, before it is measured. At the default
  threshold, `bench/loop_sum.rg`'s `run` becomes hot through its loop's back-edges during the
  first call and runs as machine code from the second.

### JIT crashes and their causes

- **Baseline JIT (issue #23): no crash was hit while building it.** The direct tests of the
  generated code (`tests/unit/jit_test.cpp`) and the conformance suite passed under ASan + UBSan
  from their first run; the only failure was a wrong expected value in a new unit test.

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
