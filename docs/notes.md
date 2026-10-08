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

**Handoff and GC protocol, as built (issue #27).** `--jit-background` (needs `--engine=jit`)
gives the JIT one compiler thread. Code: `src/jit/jit.{h,cpp}` (queue, thread, handoff),
`src/vm_reg.cpp` (`jit_count`, `jit_frame_entry`, `mark_roots`). The rules above were specified
by the owner; the details below are the implementer's and open to review.

- **Who owns what.** The engine's thread owns every heap object and every field of a function
  except one: it alone runs the VM, allocates, collects, and reads and writes `jit_hotness` and
  `jit_status`. The compiler thread owns its queue entry while compiling, the instruction words
  it generates, and a fresh `ExecBuffer` that no other thread knows about until it is published.
  The only heap memory it writes is the function's `std::atomic<JitEntry> jit_entry`. It never
  allocates on the `Heap` (which is not thread-safe); its own allocations (`std::vector`,
  `std::string`, `mmap`) go to the system allocator.
- **Queueing.** When a function reaches the threshold, `jit_count` calls `Jit::enqueue`, which
  sets `jit_status = Queued` and pushes it on a `std::deque` under a mutex, and the VM carries on
  interpreting it. The compiler thread waits on a condition variable, pops one function, sets
  `compiling_` to it, and releases the lock while it compiles.
- **What the compiler reads, and why that needs no lock.** `function.reg` (instructions,
  constants, frame size), `function.name->chars` (for the log) and the `kind` byte of constant
  objects (the whitelist rejects string constants). The register compiler builds all of these
  for every function before the program starts, and nothing changes them afterwards: the VM's
  run-time state lives in other fields (`global_cache`, `jit_hotness`, `jit_status`), and the
  VM's own reads of the bytecode while it keeps interpreting the function are reads too.
- **The handoff.** The compiler writes the code into its `ExecBuffer`, flushes the instruction
  cache over it, then stores the entry pointer with `memory_order_release`. Before every call
  of a compiled function the engine's thread loads it with `memory_order_acquire`, in C++
  (`jit_frame_entry`). If it sees the pointer, everything the compiler did before the store (the
  code bytes, the cache maintenance) happens-before the call. With a plain store, the pointer
  could become visible before the bytes (compiler or CPU reordering), and the engine would jump
  into half-written code. The first time the engine's thread sees code for a `Queued` function,
  `Jit::adopt` executes an `ISB` (the ARM architecture requires the core that runs code another
  core wrote to discard anything it may have fetched early) and sets `jit_status = Compiled`.
- **What ThreadSanitizer can and cannot see.** TSan instruments C++ loads and stores; it cannot
  see generated code, and instruction fetches are not loads. So the design keeps every shared
  access in C++: the queue and counters under the mutex, the pointer as an atomic, and machine
  code that reads and writes only the register file, which the compiler thread never touches.
  Two details make the code bytes themselves visible to TSan. (1) `ExecBuffer::write` copies the
  words with one 4-byte store each, not `memcpy`: a two-thread probe on macOS showed TSan does not
  report a race between a `memcpy` and a later unsynchronised read of the same bytes, while it
  does with plain stores. (2) `adopt` reads the code's first word as data, in C++ (also a sanity
  check: no generated code starts with 0, `udf #0`). The publishing store is made *before* the
  compiler takes the mutex to record its result, because the mutex would otherwise order the two
  threads whenever the engine's thread next takes it (to queue a function, or to collect) and hide
  a broken atomic. Checked by breaking it on purpose (not committed): with the store weakened to
  `memory_order_relaxed`, the stress unit test ("code is published while the collector runs")
  reported a data race in 5 of 5 runs, and `conformance-jit-background` in the `tsan-goto-nanbox`
  build failed 1 test; with the engine's load weakened to relaxed instead, 1 to 3 tests failed in
  each of 3 runs. With both as committed, nothing is reported.
- **The GC.** The collector is precise and stop-the-world on the engine's thread (D10) and does
  not stop for the compiler. A queued or in-progress function is a GC root: the engine's root
  marker calls `Jit::mark_pending`, which marks every queued function and `compiling_` under the
  mutex. Marking a function also marks its name and constants (`Heap::trace`), so nothing the
  compiler reads can be freed while it reads it. The collector runs concurrently with the
  compiler without a data race because what it writes and what the compiler reads are disjoint:
  in heap objects it writes only headers (`marked`, `next`, separate fields from `kind`), and it
  frees only unmarked objects; the compiler reads no header field but `kind`, which never
  changes. The
  compiler publishes while the function is still in `compiling_`, then clears `compiling_` under
  the mutex; after that it never touches the function, so a collection may free it (its machine
  code stays mapped, owned by the JIT, until the engine is destroyed, D16). Rejected alternatives:
  making the collector wait for the compile in progress (that puts the compile pause back on the
  engine's thread, which is what this engine exists to remove), and copying the bytecode at
  enqueue time (constants hold pointers to strings, so the function would still have to be
  pinned, and the copy is work on the engine's thread). `conformance-jit-background-gc-stress`
  runs the suite with a collection on every allocation while the compiler works.
- **Rejected functions** stay `Queued` (the compiler never writes `jit_status`), so they are
  never queued again and their `jit_entry` stays null: the VM keeps running them.
- **Shutdown.** `Jit::shutdown` (called by the engine's destructor before it removes its root
  marker, and by the JIT's destructor) sets a stop flag, drops what is still queued (logged as
  `[jit] cancelled NAME: still queued at exit`), lets the compile in progress finish and be
  recorded, and joins the thread. `main` destroys the engine before it returns, so the process
  never exits with the compiler thread running.
- **Scheduling (macOS).** Each queued function carries the QoS class of the thread that queued
  it, and the compiler thread switches to that class before compiling it. In bench mode the
  engine's thread is `QOS_CLASS_USER_INTERACTIVE` (D5), so the compiler runs at the same class
  and macOS prefers a performance core for it too; otherwise the measurement would compare
  compiling inline with compiling on an efficiency core.
- **Reporting.** `--jit-log` lines from the compiler thread end in `on the compiler thread`;
  `--stats` adds `on the compiler thread, N queued or in progress`. Bench mode's
  `jit_compile_ns` is the compiler thread's total, so for the background configuration it is
  time spent off the engine's thread, not a pause.
- **Cost in synchronous mode.** `jit_entry` is atomic in both modes, so `--engine=jit` without
  `--jit-background` now does an acquire load before each call where it did a plain load (in the
  `release-goto-nanbox` binary, an `ldapr` instead of an `ldr`). Row `09_jit` was first measured
  before this change and was re-measured after it (2026-10-08, commit `12d2c10368`); the data
  shows no slowdown, but cannot isolate a cost smaller than the difference between two
  invocations (§5, Engine 4, "Re-measured").
- **CI.** The `tsan` preset has no JIT (it is not NaN-boxed), so a `tsan-goto-nanbox` preset was
  added, and its CI jobs (macOS arm64 and Linux arm64) run the unit tests and the whole
  conformance suite with `--engine=jit --jit-background --jit-threshold=1`, alone, with
  `--gc-stress` and with every register-VM rung. The tree-walker's deep-recursion problem under
  TSan (see `CMakeLists.txt`) does not apply: the register VM's calls use its own frame array,
  not the native stack.
- **Measurement design (step 2).** `bench/warmup.rg` (64 distinct functions, all compiled, all
  hot in the 4th call of `run()`; its header explains the sizing), and two configurations that
  are not ladder rows (D4 fixes the ladder at nine): `jit_sync` (row 09's arguments) and
  `jit_background` (the same plus `--jit-background`), measured together over every benchmark so
  the comparison is interleaved. `ladder.py` shows their p50, p99 and max per call in a separate
  table. The result is in §5, Engine 5 ("Measured").

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

- **Other rungs.** `--engine=jit` is the register VM plus machine code, so it accepts the
  register VM's rungs (the ladder is cumulative, D4): `--fold` (a front-end pass, nothing to do
  with the JIT), `--inline-cache` (the VM's global cache, which machine code never touches:
  functions that use globals are not compiled) and `--superinstructions`. The suite runs as
  `conformance-jit-fold`, `conformance-jit-superinstructions` and `conformance-jit-all-rungs`
  (all three flags), and `bench-check-jit-all-rungs` checks the benchmarks that way.
- **Superinstructions are compiled as their two halves.** A fused word (rung 3d, §5) is the first
  instruction of a pair with only its opcode byte changed, and the next word is the second
  instruction, unchanged (`register_code.h`, "Fusion"). Fusion saves a dispatch, and machine
  code has no dispatch to save, so the JIT reads every word as the instruction it was before
  fusion (`unfused_op`: `fused_first` for a fused word, the opcode itself otherwise) and
  generates exactly the code it would for the unfused function; a unit test checks the words are
  identical. The whitelist judges the halves the same way, so fusion never changes which
  functions compile, and a rejection names the half that is outside it. Because no word moves,
  bail-out indices keep their meaning: a failed check in a first half returns the fused word's
  index and the VM re-runs the whole pair (the first half had written nothing); a failed check in
  a second half returns that word's index and the VM runs it as the plain instruction it still
  is, which is what the VM already does when a jump lands on a second half. The other option,
  rejecting every function that contains a fused word, was not taken: with
  `--superinstructions` on (it is on in row 09) `loop_sum` itself is fused, so the JIT row would
  compile nothing. The VM's back-edge count is in the shared `JUMP` body, so the `JUMP` half of a
  fused `ADD` + `JUMP` counts too.
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
- **Other generated tables.** The README's one-paragraph summary at the top is generated by
  `ladder.py` too (between the `summary:` markers) and checked by the same `--check`. JIT rows get
  a compile-time table, from the `jit_compile_ns` that `rung --bench` reports, and a `‡` on every
  cell of a benchmark in which the JIT compiles nothing (`bench/jit_not_compiled.json`; the
  `bench-jit-coverage` ctest checks that list against `rung --jit-log` in the
  `release-goto-nanbox` build, so the mark cannot outlive what the JIT does).
- **Configurations that are not ladder rows.** An entry in `scripts/ladder_configs.json` with
  `"ladder": false` (the background-compilation pair, D8) is measured by `bench.py` like any
  other, but `ladder.py` keeps it out of the speedup tables and the summary and shows it in its
  own table (p50, p99 and max per call), which says whether its configurations came from one
  invocation (interleaved) or not. The speedup tables show a benchmark's column only once a
  ladder row has measured it, so `warmup`, added after the rows were recorded, adds no column of
  "n/a".

### How the recorded rows were taken (read before comparing rows)

- Rows `01_tree` to `08_fold` were measured on 2026-10-07 and row `09_jit` on 2026-10-08, all on
  the Apple M1 Pro of D5, on AC power, from a clean tree, 10 runs of 20 `run()` calls each (the
  Provenance table in the README).
- **Four invocations of `bench.py`, not one.** Each results file records when it was taken,
  and the committed files come from four different times: `01_tree`; `02_stack` to `06_super`;
  `07_ic` with `08_fold`, re-measured after two earlier runs had flagged noise; and `09_jit`,
  re-measured on 2026-10-08 together with the background-compilation pair `jit_sync` and
  `jit_background` (§5, Engine 4, "Re-measured"). Interleaving
  (§5, the measurement pipeline) only protects comparisons inside one invocation, so three
  marginal ratios compare rows from different invocations: `02_stack` against `01_tree`,
  `07_ic` against `06_super`, and `09_jit` against `08_fold` (also a different day). Every
  cumulative ratio compares against `01_tree`, which was measured on its own.
- **Three commits, two sources.** Rows `07_ic` and `08_fold` record commit `0d05a706c5`, row
  `09_jit` `12d2c10368`, the others `e618db54a8`. `0d05a706c5` is an interim commit of the
  earlier results files on top of `e618db54a8`; it changes only `README.md` and `results/`, so
  rows 01 to 08 ran the same source. `12d2c10368` is the background-compilation commit (issue
  #27): it changes the JIT's source as well (`src/jit/`, the JIT's hooks in `src/vm_reg.cpp`, the
  atomic `jit_entry`), so row `09_jit` against `08_fold` compares different sources as well as
  different invocations.
- **Instruction counts.** `docs/instruction_counts.txt` (written by
  `scripts/instruction_counts.py`, and checked by the `instruction-counts` ctest in the `debug`
  build) has how many instructions each VM dispatches per benchmark. They are counts, not times,
  and they explain much of what follows; they do not depend on the dispatch style or the value
  representation, so rows 02 to 04 share one count, and rows 06 to 08 another.

### Engine 1: tree-walker

- **Measured.** Row `01_tree` is the baseline, so its speedup is 1.00× by definition; what it
  contributes is the denominator of every cumulative ratio, and its own per-call times are in the
  results file and its p99s in the README's p99 table.
- **What dominates its time: expectation, not measurement.** No profile has been taken, so the
  split below is what the code makes likely, not a measured breakdown:
  - *An allocation for every block and every call.* Each block that runs and each call creates an
    `ObjEnvironment` on the garbage-collected heap, which holds its variables in a
    `std::unordered_map` (see the scope bullet below). This part is counted, not guessed: the
    results file's `heap` counters for `loop_sum` show 40,000,030 objects allocated per process
    (20 calls of a loop of 2,000,000 iterations, so one environment per iteration of the loop
    body) and 2,443 collections, where the stack VM allocates 11 objects and never collects.
    `sieve` allocates 37,007,314 objects, `closures` 36,000,080 and `fib` 12,712,451. What
    share of the time the allocation and the collections take is not measured.
  - *Two hash lookups for every variable access.* `eval_node(Variable)` first maps the name's
    `string_view` to its interned `ObjString*` through `names_` (an `unordered_map` keyed by the
    string, so the name's bytes are hashed every time), then walks `hops` environments and looks
    the pointer up in that environment's `vars`. The VMs replace both with an array index.
  - *`std::visit` on every node.* Every expression and statement evaluation dispatches through a
    `std::variant` visit, the tree-walker's equivalent of the VMs' dispatch, with the tree's
    pointer chasing on top.
- **What the later rows suggest about it.** The stack VM does none of the three, and is 3.80× to
  4.99× as fast on five benchmarks; on `strcat` it is 1.30× as fast, because that benchmark's
  time is mostly the string copying that both engines share (see the stack VM entry).
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

### Engine 2: stack VM

- **What it is.** Row `02_stack` (`release`, `--engine=stack`): the program is compiled once to
  stack bytecode (`compiler_stack.cpp`, following *Crafting Interpreters*' `clox`) and run by a
  `switch` dispatch loop (`vm_stack.cpp`) over 16-byte tagged values (D4 row 2).
- **Expected.** No expectation was written here before it was measured, so this one is written
  after the numbers were known and should be read that way. The design removes all three of the
  tree-walker's likely costs: variables become slots in an array (no hash lookups), blocks and
  calls allocate nothing (a frame is a window on the value stack), and `std::visit` on every node
  becomes one `switch` per instruction. What it cannot remove is work in the shared runtime
  (D10): string concatenation, array accesses, the collector. So it should help most on the
  benchmarks that are pure interpretation overhead and least on `strcat`.
- **Measured** (README tables; every cell's IQR is under 2% of its median, none is noisy):

  | | fib | loop_sum | sieve | nbody | strcat | closures |
  |---|---|---|---|---|---|---|
  | `02_stack` over `01_tree` | 3.80× | 3.80× | 4.99× | 4.70× | 1.30× | 4.04× |

- **Where the speedup comes from, per benchmark** (counts from the results files' `heap`
  counters, per process of 20 calls, and from `docs/instruction_counts.txt`; no time breakdown
  was measured, so the attributions are the most likely reading of the counts, not a profile):
  - `loop_sum`, `fib`: the tree-walker allocates 40,000,030 and 12,712,451 objects, the stack VM
    11 and 14, and neither VM run collects. The stack VM dispatches 40,000,013 instructions for
    one call of `loop_sum` (20 per iteration of its loop) and 7,627,457 for `fib`. Both gain
    3.80×.
  - `sieve` gains the most (4.99×). The tree-walker allocates 37,007,314 objects (an environment
    for each pass through each loop body and each `if` block), the stack VM 31, 20 of which are
    the arrays (one per call); peak RSS falls from 30,703,616 to 11,485,184 bytes.
  - `nbody` (4.70×): 15,203,116 objects for the tree-walker, 143 for the stack VM.
  - `closures` (4.04×) gains less than the first four because both engines allocate per
    iteration: the program creates a closure and captures a variable every time round, so the
    stack VM still allocates 12,000,061 objects and runs 596 collections (the tree-walker:
    36,000,080 and 2,063).
  - `strcat` gains only 1.30×. It allocates in both engines: 80,174 objects, 484,253,448 bytes
    and 464 collections in the stack VM; 6,764,834 objects and 912,099,234 bytes in the
    tree-walker. Almost all the stack VM's bytes are the second loop, which appends `"xyz"` 4,000
    times to a string that grows to 12,000 characters: from the source, one call copies
    3 × (1 + 2 + ... + 4,000) = 24,006,000 characters, and every `+` builds a `std::string`, copies
    both operands into it and interns the result (`op_add`, `src/runtime/ops.cpp`), which hashes
    it. That code is the same in every engine (D10), and the stack VM dispatches only 5,674,024
    instructions for a whole call. That this shared work dominates is a hypothesis consistent
    with every later row (see rung 3d: 36.5% fewer dispatches changed its time by 1.00×), not a
    measurement.
- **Expected versus measured.** The ranking matches the expectation: pure interpretation
  overhead (`sieve`, `nbody`, `loop_sum`, `fib`) gains 3.80× to 4.99×, allocation-bound
  `closures` less, string-bound `strcat` least.

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
- **Measured** (row `03_goto` over `02_stack`, both from the same `bench.py` invocation, so
  interleaved; no cell is noisy):

  | | fib | loop_sum | sieve | nbody | strcat | closures |
  |---|---|---|---|---|---|---|
  | `03_goto` over `02_stack` | 1.19× | 1.37× | 1.81× | 1.31× | 1.03× | 1.15× |

- **Expected versus measured.** The expectation allowed for a small gain; the measured gain is
  real on five of six benchmarks, from 1.15× (`closures`) to 1.81× (`sieve`). `strcat` (1.03×)
  barely moves, as it does on every rung (see the stack VM entry). The instruction counts are
  identical in both rows (same bytecode), so the gain is in the cost of each dispatch.
- **What the data does not show.** Why each dispatch got cheaper. The goto build changes more
  than the number of indirect branches: the disassembly above also loses the bounds check
  (`cmp w8, #0x23; b.hi`) and the 2-byte jump-table offset arithmetic on every dispatch, and the
  compiler allocates registers and lays out a differently shaped function. Better prediction and
  fewer instructions per dispatch cannot be told apart without branch-miss and
  instruction-retired counts (D5's optional Instruments counters, not taken). Nor does it show
  why `sieve` gains so much more than the others; note that `sieve` is also the benchmark that
  loses the most at the next rung.
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
- **Measured: a regression on every benchmark** (row `04_nanbox` over `03_goto`, the same
  `bench.py` invocation, interleaved; no cell is noisy, every IQR is under 1.4% of its median):

  | | fib | loop_sum | sieve | nbody | strcat | closures |
  |---|---|---|---|---|---|---|
  | `04_nanbox` over `03_goto` | 0.90× ▼ | 0.74× ▼ | 0.63× ▼ | 0.78× ▼ | 0.98× ▼ | 0.91× ▼ |

  The NaN-boxed stack VM is slower than the tagged one on all six, by 2% (`strcat`) to 37%
  (`sieve`). It gives back most of what computed goto gained on `loop_sum` and `sieve`, and the
  stack VM with both rungs is slower than with computed goto alone. Because the comparison is
  interleaved and the spread is small, this is not noise.
- **The memory side behaved as expected** (results files, `heap` counters and peak RSS per
  process): `sieve`'s 20 arrays went from 192,002,178 to 96,002,018 bytes allocated and its peak
  RSS from 11,452,416 to 6,701,056 bytes; `closures` from 624,004,410 to 576,004,250 bytes
  allocated (596 to 550 collections). The other benchmarks' peak RSS hardly changes (for
  example `nbody` 2,113,536 to 2,162,688 bytes), as expected: their values do not fill
  pages.
- **Expected versus measured.** Expected: a gain from moving half the bytes, largest where large
  arrays are touched, against an extra mask-and-compare per type check. Measured: the bytes did
  halve, and the time got worse everywhere, worst on `sieve`, the benchmark with the largest
  array, which is the opposite of the prediction. So the cache benefit, if any, is outweighed.
- **What the data does and does not show.** It shows that the same bytecode (the instruction
  counts do not depend on the value representation, `docs/instruction_counts.txt`), with half
  the memory traffic, runs slower in this build. It does **not** show why: no profile,
  disassembly comparison or hardware counters were taken for this rung. Hypotheses, none of them
  tested:
  - *Each type check and unbox costs more.* The tagged struct tests one tag byte; the NaN-boxed
    `is_int` compares the upper bits against a mask, `as_int` extracts the low 32 bits, `make_int`
    and `make_float` build a value (the latter with a NaN check), and the stack VM's handlers do
    these for every operand of every instruction. `sieve` is mostly index checks and bool tests,
    so it would pay the most often, which fits the ranking, but nothing measured it.
  - *A different compiled loop.* The two builds compile `StackEngine::execute` with a different
    `Value` type, so register allocation and code layout differ throughout the function, and the
    previous rung showed that dispatch on this machine is sensitive to such details (`sieve`
    gained 1.81× from computed goto). Part of the swing may be layout rather than the
    representation itself.
  - *Not memory-bound.* The only large data structure in the suite is `sieve`'s array of
    600,001 values (from the source: 9,600,016 bytes tagged, 4,800,008 NaN-boxed), and it is
    walked in increasing index order (with a stride in the marking loop), which hardware
    prefetching handles well whatever the element size. The other five benchmarks never have
    more than 1,059,915 bytes live on the heap (`peak_live_bytes`, `02_stack`). If no benchmark
    was waiting on memory,
    halving the value size had little to win, and the expectation overrated it.
  What would decide between them: compare the disassembly of a hot handler (`ADD`, `LT`,
  `INDEX_GET`) in `release-goto` and `release-goto-nanbox`, and take instructions-retired and
  branch-miss counts for one benchmark in both builds (D5's optional Instruments counters).
- **What it means for the rows above it.** D4 keeps NaN-boxing in every register-VM row and the
  JIT needs it (D3, D16), so rows 05 to 09 all run on NaN-boxed values and there is no
  measurement of the register VM with tagged values. Whether the register VM pays the same
  penalty is unknown. D4's optional "leave one out" row (the register VM on `release-goto`)
  would measure it; whether to add it is the owner's call.

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
  benchmarks did not exist when this was written, so the programs are conformance tests; the
  per-benchmark counts are under **Measured** below:

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
- **Measured** (row `05_register` over `04_nanbox`, the same invocation, interleaved; no cell is
  noisy), next to the instruction counts of the two VMs for the same benchmark
  (`docs/instruction_counts.txt`: the script's top level plus one call of `run()`):

  | Benchmark | Speedup over `04_nanbox` | Stack VM instructions | Register VM instructions | Register / stack |
  |---|---|---|---|---|
  | `sieve` | 2.36× | 29,901,158 | 9,902,205 | 0.33 |
  | `loop_sum` | 1.75× | 40,000,013 | 14,000,008 | 0.35 |
  | `fib` | 1.41× | 7,627,457 | 4,131,542 | 0.54 |
  | `nbody` | 1.39× | 22,578,652 | 8,491,383 | 0.38 |
  | `closures` | 1.20× | 17,100,029 | 9,600,020 | 0.56 |
  | `strcat` | 1.03× | 5,674,024 | 1,910,017 | 0.34 |

- **Expected versus measured.** Both halves of the expectation hold. The register VM dispatches
  0.33 to 0.56 times as many instructions, and calls are identical (one `Calls` column). Time falls
  by less than the instruction count on every benchmark, so each register instruction costs more
  than a stack one, as the wider decode predicted; how much more is not measured. Spec §5.5's
  "largest single win of the ladder" holds among rungs 3a to 4 for `sieve`, `nbody` and
  `closures`, but not for `fib` (inline caching gains more, 1.46×) or `loop_sum` (the JIT,
  3.22×); leaving the tree-walker (row 02) is larger than any of them.
- **`strcat` is the control.** It dispatches 0.34 as many instructions and gains 1.03×, so
  dispatch was never a meaningful part of its time, consistent with the stack VM entry.
- **Caveat.** The row below this one is the NaN-boxed stack VM, which regressed (rung 3b), and
  the register VM has only been measured with NaN-boxed values. How much of each ratio above is
  the register design and how much is the register VM avoiding whatever made the NaN-boxed stack
  VM slow is not separable from these rows.

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
- **Measured** (row `06_super` over `05_register`, the same invocation, interleaved; no cell is
  noisy, though `loop_sum`'s IQR, 4.45% of its median, is close to the 5% line):

  | Benchmark | Speedup over `05_register` | Fewer dispatches (table above) |
  |---|---|---|
  | `loop_sum` | 1.50× | 42.9% |
  | `sieve` | 1.24× | 37.4% |
  | `nbody` | 1.04× | 24.7% |
  | `closures` | 1.04× | 12.5% |
  | `fib` | 1.01× | 15.4% |
  | `strcat` | 1.00× | 36.5% |

- **Expected versus measured.** Where dispatch is the work, the rung did what was expected:
  `loop_sum` and `sieve`, the two benchmarks with the largest cut in dispatches, gained most, and
  `fib` and `closures`, dominated by calls, least. The exception is `strcat`: 36.5% fewer
  dispatches and no change in time (1.00×), which the expectation got wrong. It is the strongest
  evidence in the ladder that `strcat`'s time is not in the interpreter at all (stack VM entry).
  `nbody` gained 1.04× from 24.7% fewer dispatches, so its time is mostly in the handlers' work
  (float arithmetic and array accesses in `src/runtime/`), not in dispatch; that reading is
  inferred from the two numbers, not profiled.
- **Unfused loop versus old loop.** Row `05_register` was measured once, on this build, so there
  is no earlier measurement to compare it with. The next rung's `loop_sum` result suggests the
  register VM's time on that benchmark does depend on how the dispatch function is compiled
  (rung 3e).
- **The unfused loop is not exactly the old loop.** The fused handlers are in the same dispatch
  function as the plain ones. Without the flag they are never reached, but the compiler may lay
  the function out differently (more labels, more code), so the previous row can move for reasons
  unrelated to this change. Unlike the inline cache, there was no way to keep the plain loop
  identical without duplicating the loop. If row 05 is measured on this build and differs from
  an earlier measurement, say so.
- **What the ladder rows must pass.** D4 makes the ladder cumulative, so `--superinstructions`
  belongs in the arguments of every row from 06 on: `06_super` is
  `--engine=register --superinstructions`, `07_ic` is `--engine=register --superinstructions
  --inline-cache`, and `08_fold` is `--engine=register --superinstructions --inline-cache
  --fold`, or it would measure folding without the two rungs below it. All of these rows are in
  `scripts/ladder_configs.json` (the arguments of `08_fold` gained the two earlier flags when
  rows 02 to 07 were added), and `tests/test_bench_scripts.py` checks that the file is exactly
  this cumulative ladder. The three flags compose, and the suite runs with all three at once
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
- **Measured** (row `07_ic` over `06_super`; the two rows come from different `bench.py`
  invocations, so this comparison is not interleaved):

  | | fib | loop_sum | sieve | nbody | strcat | closures |
  |---|---|---|---|---|---|---|
  | `07_ic` over `06_super` | 1.46× | 0.69× ▼ † | 1.00× | 1.00× | 1.00× ▼ | 0.98× ▼ |

- **`fib`: a larger gain than expected.** The expectation above said a large gain on `fib` would
  be surprising; it gained 1.46×, the largest gain any rung after the register VM gives `fib`.
  Every call in `fib` goes through `GET_GLOBAL`, so the hash lookup it removes was a much larger
  share of a call than "a hash, a bucket walk and a key comparison against a whole `CALL`"
  suggested. Why it was so expensive is not measured. A hypothesis: libc++'s
  `std::unordered_map` hashes the pointer with a mixing function, reduces the hash to a bucket
  with an integer division when the bucket count is not a power of two, and then follows a
  pointer to the bucket's node, so a lookup is a dependent chain of several slow steps that the
  cache's single load replaces.
- **`sieve`, `nbody`, `strcat`, `closures`: no change**, as expected (at most 2%: `closures`,
  0.98×, also has 300,001 `GET_GLOBAL`s per call and might have gained slightly; it lost 2%
  instead). These four comparisons cross invocations, and a 2% difference between invocations
  is within what this setup can resolve, so they are reported as measured but not interpreted.
- **`loop_sum`: 0.69× and flagged noisy; the figure is uncertain.** `loop_sum` executes no
  `GET_GLOBAL` at all (the table above), so the cache does nothing in it, and the expectation
  said any difference would be noise. The row was measured three times (two discarded runs and
  the committed one), and `loop_sum` was flagged noisy every time; the committed IQR is 11.52%
  of the median. What the committed per-run totals show (10 runs of 20 calls each):
  - `07_ic`: two clusters, four runs from 720,907,083 to 756,167,125 ns and six from
    849,202,208 to 862,602,414 ns. The noise flag is this split, not random scatter.
  - `08_fold` (the same flags plus `--fold`, which changes nothing in `loop_sum`: 0 expressions
    folded, identical bytecode) shows the same two clusters: two runs at 751,384,875 and
    753,210,666 ns, eight from 849,316,291 to 858,743,836 ns. Both of its quartiles fall in the
    slow cluster, so it is not flagged, but it is the same picture.
  - `06_super`: every run from 569,440,166 to 675,615,124 ns, so even `07_ic`'s fast cluster is
    slower than every `06_super` run. The direction (slower with `--inline-cache`) looks real;
    its size is not known.
  - `09_jit`, which runs the same dispatch loop with the inline cache before `loop_sum`'s `run`
    is compiled, has a first call that varied from 30,989,875 to 43,628,083 ns across runs as
    first recorded, and from 30,835,958 to 38,966,208 ns when re-measured (Engine 4). The
    background-compilation pair shows the same grouping of first calls (Engine 5, "Measured").

  What is known about the cause: `--inline-cache` makes the VM run a separately compiled copy of
  its dispatch loop (`execute_loop<true, ...>` instead of `<false, ...>`, so that the row below
  is not slowed by a flag test). The handlers `loop_sum` runs are the same source in both, but
  not the same machine code. Hypotheses, untested: the copy with the cache is compiled with a
  worse register allocation or layout for `loop_sum`'s three fused handlers; and, because the
  speed differs between processes running the same binary, something placed differently in
  each process matters (macOS moves the whole binary by whole pages, which keeps the code's
  alignment, so the suspect is data placement, such as where the register file or the heap
  lands). What would decide it: disassemble `MOD_ADD`, `LT_JUMP_IF_FALSE` and `ADD_JUMP` in
  both instantiations, and run `loop_sum` in many fresh processes with and without
  `--inline-cache` (a measurement, so the owner's).
- **Consequence for the rows above.** Rows `08_fold` and `09_jit` carry `--inline-cache` (D4 is
  cumulative), so their `loop_sum` cells are measured against, or interpreted in, the same loop:
  `08_fold` over `07_ic` on `loop_sum` (1.00×, †) compares two rows with the same bimodal
  behaviour.
- **What the ladder rows must pass.** D4 makes the ladder cumulative, so `--inline-cache` belongs
  in the arguments of every row from 07 on. `scripts/ladder_configs.json` has `07_ic`
  (`--engine=register --superinstructions --inline-cache`), and `08_fold` and `09_jit` carry the
  flag too, so no row measures folding or the JIT without the rungs below it. `--fold` and
  `--inline-cache` compose, and the conformance suite runs with both at once
  (`conformance-register-fold-inline-cache`).
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
- **Measured** (row `08_fold` over `07_ic`, the same invocation, interleaved; `08_fold` is
  `--engine=register --superinstructions --inline-cache --fold`, D4 being cumulative):

  | | fib | loop_sum | sieve | nbody | strcat | closures |
  |---|---|---|---|---|---|---|
  | `08_fold` over `07_ic` | 0.99× ▼ | 1.00× ▼ † | 1.00× ▼ | 1.00× | 1.00× ▼ | 1.01× |

- **Expected versus measured.** As expected: nothing to fold in five benchmarks, ten folds
  outside `nbody`'s hot loop, and every cell within 1% of 1.00×. The `▼` marks on the `1.00×`
  cells mean the ratio is just below 1 before rounding; a difference that small is not a
  result of the pass, which changes no instruction those benchmarks run
  (`docs/instruction_counts.txt`: the `+ super` and `+ all` columns are equal, except `nbody`'s,
  4 instructions fewer in the counted run).
  The `loop_sum` cell carries `07_ic`'s noise flag (rung 3e).

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
  the counting on each call.
- **The row.** `09_jit` is `release-goto-nanbox`, `--engine=jit --superinstructions
  --inline-cache --fold`: rows 6 to 8's rungs plus the JIT (D4 is cumulative); the JIT is the
  register VM plus machine code, so it accepts all three.
- **What compiles in the benchmarks** (`rung --engine=jit --superinstructions --inline-cache
  --fold --jit-log --bench=20`, the `09_jit` row's arguments at the default threshold; the
  `bench-jit-coverage` ctest re-runs this and fails if which functions compile changes; these
  are facts about the code, not timings):

  | Benchmark | Compiled | Rejected (first instruction outside the whitelist) |
  |---|---|---|
  | `loop_sum` | `run`: 11 bytecode instructions → 76 machine instructions, 304 bytes | none |
  | `fib` | none | `fib`: instruction 3, `GET_GLOBAL` (its recursive call, D7); `run` is called 20 times and never reaches the threshold |
  | `sieve` | none | `run`: instruction 1, `GET_GLOBAL` (the native `array`) |
  | `nbody` | none | `root`: instruction 1, `GT` with a float constant; `advance`: instruction 0, `GET_GLOBAL` (`len`); `run`: instruction 0, `GET_GLOBAL` |
  | `strcat` | none | `run`: instruction 4, `LOADK` of a string constant |
  | `closures` | none | `run`: instruction 4, `GET_GLOBAL`; `make_adder`: instruction 0, `CLOSURE`; `add` and `next`: instruction 0, `GET_UPVALUE` |

  One function in the whole suite compiles. In the other five benchmarks every function that
  gets hot is rejected, so the `09_jit` row runs them in the register VM; the README marks those
  cells with `‡`. The compiled function never bailed out (`--stats`: 0 bail-outs).
- **Measured** (row `09_jit` as re-measured on 2026-10-08, see "Re-measured" below, over
  `08_fold` from 2026-10-07; different invocations, days and commits, so not interleaved):

  | | fib | loop_sum | sieve | nbody | strcat | closures |
  |---|---|---|---|---|---|---|
  | `09_jit` over `08_fold` | 1.00× ▼ ‡ | 3.22× | 0.98× ▼ ‡ | 0.99× ▼ ‡ | 1.01× ‡ | 1.00× ▼ ‡ |
  | `09_jit` over `01_tree` | 8.33× ‡ | 22.64× | 16.53× ‡ | 6.88× ‡ | 1.35× ‡ | 5.25× ‡ |
  | JIT time per process, median of 10 runs | 2.5 µs ‡ | 16.4 µs | 2.9 µs ‡ | 5.2 µs ‡ | 2.6 µs ‡ | 6.3 µs ‡ |

  The last line is the README's compile-time table: `jit_compile_ns`, the total time one process
  spent in `Jit::compile`, rejected functions included (the whitelist check runs inside the same
  timer). For `loop_sum` it is 0.0062% of the median total time of the 20 timed calls.
- **Re-measured, and why.** Row `09_jit` was first recorded on 2026-10-07 at commit
  `e618db54a8` (the `results/09_jit.json` committed in `04e96e6`). Background compilation (issue
  #27, commit `12d2c10368`) then made `jit_entry` a `std::atomic`, so every call in `--engine=jit`
  now does an acquire load (`ldapr`) where it did a plain load (`ldr`), background thread or not
  (D8, "Cost in synchronous mode"). The row was re-measured at `12d2c10368`, in the same
  `bench.py` invocation as `jit_sync` and `jit_background` (Engine 5), and the table above is the
  re-measured row. Median total time of the 20 timed calls, first recording against
  re-measure:

  | | fib | loop_sum | sieve | nbody | strcat | closures |
  |---|---|---|---|---|---|---|
  | first recording (ns) | 275,149,957 | 272,949,332.5 | 334,446,958 | 663,369,624.5 | 2,740,716,939.5 | 917,861,583 |
  | re-measured (ns) | 274,670,020.5 | 264,306,959 | 333,120,455.5 | 664,691,542.5 | 2,733,530,522.5 | 918,024,063 |
  | re-measured / first | 0.9983 | 0.9683 | 0.9960 | 1.0020 | 0.9974 | 1.0002 |

  The cumulative ratios moved by at most 0.07 except `loop_sum`'s (21.92× to 22.64×), the
  marginal ones by at most 0.10 (`loop_sum`, 3.12× to 3.22×), and `fib`'s marginal ratio went
  from 0.99× to 1.00×.

  What the data shows: no slowdown. The re-measured row is slower than the first recording on two
  benchmarks, by 0.20% (`nbody`) and 0.02% (`closures`), and faster on the other four. The acquire
  load runs on every call of every function, compiled or not (`jit_frame_entry`), so `closures` and
  `fib`, which make the most calls (900,002 and 635,622 in one `run()` with the top level,
  `docs/instruction_counts.txt`) and compile nothing, are where a cost would show most: `closures`
  is 0.02% slower and `fib` 0.17% faster. What it cannot show: a cost smaller than the difference
  between two invocations. The two recordings are different invocations on different days, not
  interleaved, and different commits: `12d2c10368` changes more than the load (the `background()`
  test in `jit_count`, and `ExecBuffer::write` copying code a word at a time, D8), so a difference
  of a few tenths of a percent cannot be attributed to any one change. For scale, `09_jit` and
  `jit_sync` have identical arguments and were measured in the same invocation, and their median
  totals differ by up to 0.20% (`nbody`). `loop_sum`'s 3.2% is its first call (below), not the load:
  the load runs 20 times per process there, and the compiled calls' p50 went from 12.30 ms to 12.25
  ms.
- **`loop_sum`: where the 3.22× comes from, call by call.** Each run is a fresh process and calls
  `run()` 20 times. `run` becomes hot through its loop's back-edges early in the first call and is
  compiled then, on the main thread; without on-stack replacement that call finishes in the VM, and
  machine code runs from the second call on. The per-call times in the results file show exactly
  that, in every run: the first call took 30,835,958 to 38,966,208 ns, the other 19 took 11,885,833
  to 12,523,042 ns. So the p50 (12.25 ms) is a compiled call and the p99 (37.84 ms) is a first call,
  which runs in the VM. The 3.22× compares medians of totals that each include one such call; for a
  compiled call alone, compare the p50s: 42.55 ms for `08_fold`, 12.25 ms for `09_jit`. The first
  call's time varies between processes in groups (around 31 ms in six runs, 37.7 to 39.0 ms in
  four), and every one of them is shorter than `08_fold`'s p50, although no machine code runs in it.
  It is the same kind of per-process grouping that rung 3e found in `07_ic` and `08_fold`, and like
  that one it is not explained. In the first recording the first call took 30,989,875 to 43,628,083
  ns and the p99 was 43.32 ms, which is why `loop_sum`'s median total moved most between the two
  recordings.
- **`fib` and the other four: unchanged, slightly below 1.00×.** As expected for `fib` (D7) and,
  now that the log shows nothing compiles, for the other four. The 1% to 2% losses (`sieve`
  0.98×) are within what a comparison across invocations can resolve. A hypothesis for a real
  part of them, not measured: the JIT's hooks stay in the dispatch loop after a function is
  rejected. Every call goes through `jit_frame_entry` and every backward jump through
  `jit_count`, which return at once for a rejected function but still cost a call and a test,
  and `--engine=jit` runs a third instantiation of the dispatch loop (`execute_loop<true, true>`),
  with the same layout caveat as rung 3e.
- **Expected versus measured.** Expected a large gain on `loop_sum` and none on `fib`; measured
  3.22× and 1.00× ▼ (re-measured; first recorded as 3.12× and 0.99× ▼). Not foreseen: that
  `loop_sum` would be the only benchmark compiled at all. The whitelist's exclusions (globals, which
  include every native such as `array` and `len`; float and string constants; closures and captured
  variables) cover at least one hot function of every other benchmark. Widening it (D7's direct
  self-recursion; reading a global through the inline cache's cell, rung 3e; float constants) is how
  the JIT would reach more of the suite; which, if any, is the owner's decision.
- **Compile times, per function, for D8 step 1.** From the results file's per-run
  `jit_compile_ns`, using the log above to say which functions each figure covers (one process
  per run, ten runs; the re-measured row, and in the last column the first recording):

  | Benchmark | Functions decided in one process | JIT time per process (10 runs) | First recording |
  |---|---|---|---|
  | `loop_sum` | `run` compiled | 13,000 to 20,750 ns, median 16,437.5 | 15,042 to 22,041 ns, median 17,687.5 |
  | `fib` | `fib` rejected | 2,167 to 5,208 ns, median 2,500 | 1,834 to 5,375 ns, median 2,458 |
  | `sieve` | `run` rejected | 2,625 to 6,667 ns, median 2,937.5 | 1,708 to 5,959 ns, median 2,646 |
  | `strcat` | `run` rejected | 2,208 to 5,416 ns, median 2,563 | 2,042 to 8,791 ns, median 5,187 |
  | `nbody` | `root`, `advance`, `run` rejected | 2,708 to 7,583 ns, median 5,208 | 2,333 to 7,583 ns, median 5,145.5 |
  | `closures` | `run`, `make_adder`, `add`, `next` rejected | 3,959 to 8,874 ns, median 6,313.5 | 5,084 to 10,085 ns, median 5,896 |
  | `warmup` | 64 functions compiled, all in the 4th call (`bench/warmup.rg`, Engine 5) | 488,293 to 536,171 ns, median 519,338.5 | not measured then |

  The ranges of the two recordings overlap on every benchmark; `strcat`'s median halved, from
  5,187 to 2,563 ns, inside an overlapping range, so the median alone says little at this size.
  `loop_sum`'s figure is the compile time of one function, `run`, because nothing else is
  decided in that process. The `--jit-log` line also prints a compile time, but from a single
  process outside a `bench.py` session, so it is not quoted here (CLAUDE.md: performance figures
  come from `results/`). A rejection stops at the first instruction outside the whitelist
  (within the first five here), yet costs microseconds; the timer also covers building the
  rejection message (a `std::string`), and a hypothesis, not measured, is that this and cold
  caches are most of it. The compile landed in the first timed call of every run (above).
  `warmup`'s 64 compiles average 8.1 µs each (the median per process divided by 64).
- **What this means for D8 (background compilation).** Written from the first recording, before
  Engine 5 was built; Engine 5's entry has what was then measured. D8 warned that a JIT this small
  might compile in well under a millisecond, too fast for a background thread to remove a visible
  pause. The data says so for this suite: the one compile takes 15,042 to 22,041 ns, inside a first
  call of at least 30,989,875 ns, and the tail that does exist in `loop_sum` (its p99) is the first
  call running in the interpreter, which a background compiler would not shorten (the code would
  still arrive during the first call and be entered on the second). Following D8's order of work,
  building the background thread should come with the warm-up benchmark D8 step 2 describes (many
  distinct hot functions, short iterations), and if that also shows no measurable tail effect, the
  result is reported as exactly that (D8 step 3). Whether to go ahead on that basis is the owner's
  decision. On-stack replacement would remove the interpreted first call, but it is outside D8 and
  not planned.

### Engine 5: background compilation

- **What changed.** `--jit-background` (D8, "Handoff and GC protocol, as built"): a function that
  reaches the threshold is queued for a compiler thread instead of compiled on the engine's
  thread, and the VM keeps interpreting it until the compiled code is published. Which functions
  compile is unchanged (the same whitelist decides on either thread); only when the code arrives
  changes. The whole conformance suite passes with `--jit-threshold=1 --jit-background`, with and
  without `--gc-stress` and with every rung, in the `asan-goto-nanbox`, `release-goto-nanbox`
  and `tsan-goto-nanbox` builds; CI runs the last on macOS arm64 and Linux arm64.
- **Expected** (written before any measurement). Engine 4's entry already found the existing
  suite gives a background thread nothing to remove: `loop_sum` is the only benchmark with a
  compiled function, its one compile lands inside a first call that is interpreted either way,
  and that first call is its p99. So on the six ladder benchmarks no change in p50, p99 or max is
  expected beyond noise. `bench/warmup.rg` is where a difference could show: all 64 functions
  compile in its 4th call. Synchronously, that call pays for 64 compiles. In the background it
  does not, but the code also arrives later, so the 4th call and possibly the 5th run longer in
  the interpreter. Whether the net effect on the slowest calls (p99, max) is a gain, a loss or
  nothing visible depends on how long a compile takes against how long the interpreter runs
  while waiting for it, which is exactly what has not been measured.
- **Measured.** `jit_sync` (row 09's arguments) and `jit_background` (the same plus
  `--jit-background`) were measured on 2026-10-08 at commit `12d2c10368`, together with row
  `09_jit`, in one `bench.py` invocation, so the three are interleaved (D5) and comparable call
  for call; 10 runs of 20 calls of `run()` each, every run a fresh process. Time of one call in
  ms (the README's "Background compilation" table; p50, p99 and max are nearest-rank over the
  200 calls pooled, so the p99 is the third-slowest call and the max the slowest):

  | Benchmark | `jit_sync` p50 | p99 | max | `jit_background` p50 | p99 | max |
  |---|---|---|---|---|---|---|
  | `fib` ‡ | 13.72 | 14.09 | 15.23 | 13.74 | 14.11 | 16.91 |
  | `loop_sum` | 12.26 | 31.71 | 39.07 | 12.24 | 43.15 | 43.21 |
  | `sieve` ‡ | 16.65 | 17.18 | 17.28 | 16.64 | 17.11 | 17.16 |
  | `nbody` ‡ | 33.29 | 33.90 | 34.34 | 33.24 | 33.64 | 33.70 |
  | `strcat` ‡ | 136.47 | 138.60 | 147.10 | 136.56 | 143.03 | 159.57 |
  | `closures` ‡ | 45.90 | 47.39 | 47.74 | 45.91 | 47.49 | 47.59 |
  | `warmup` | 0.09 | 0.82 | 0.83 | 0.09 | 0.32 | 0.35 |

- **`warmup`: a measurable tail effect, in the direction background compilation is for.** p99
  0.82 ms synchronously, 0.32 ms in the background; max 0.83 ms and 0.35 ms; p50 0.09 ms both.
  Call by call, from the per-call times in the two results files (ranges over all ten runs):

  | Call of `run()` | `jit_sync` (ns) | `jit_background` (ns) |
  |---|---|---|
  | 1 to 3 (interpreted) | 254,333 to 369,000 | 254,209 to 272,958 |
  | 4 (all 64 functions reach the threshold) | 787,291 to 831,708 | 271,625 to 350,000 |
  | 5 | 95,042 to 109,875 | 188,791 to 216,167 |
  | 6 | 90,417 to 97,708 | 98,541 to 115,708 |
  | 7 to 20 | 87,750 to 205,083 | 87,792 to 114,250 |

  Synchronously, call 4 pays for the 64 compiles on the engine's thread: the JIT time per
  process is 504,584 to 548,584 ns (median 524,397), about what call 4 takes beyond calls 1 to
  3. In the background, call 4 only queues the 64 functions and stays in the VM, and every
  background call 4 (at most 350,000 ns) was faster than every synchronous one (at least
  787,291 ns), in all ten runs: the two do not overlap, so this is not noise. The price is that
  the code arrives later. The compiler thread spends 474,039 to 559,585 ns per process (median
  521,523.5) on the same 64 compiles, more than call 4 lasts, and it starts no earlier than call
  4 and compiles one function at a time, so some functions are still waiting when call 5 starts
  and run in the VM: call 5 takes about twice as long as synchronously, and call 6 a little
  longer. The compile work is the same on either thread (medians 524,397 and 521,523.5 ns); it
  only moves. Over the 20 calls the background is ahead: median total per process 3,017,478.5
  ns synchronously, 2,610,395 ns in the background (0.865×), because the extra interpreting in
  calls 5 and 6 costs less than the compiles that left call 4.

  Two details the data shows but does not explain. Background call 4 (271,625 to 350,000 ns) is
  slower than calls 1 to 3 (254,209 to 272,958 ns), and it is still the slowest call of every run,
  so it is what the background's p99 and max are. A hypothesis, not measured: the 64 enqueues (each
  takes the queue's mutex and signals the condition variable, which can wake the compiler thread
  through the kernel), and the compiler thread running at the same time as the engine's thread (on
  this chip, the cores of one cluster share a level-2 cache). And background call 6 is slower than
  synchronous call 6; a hypothesis, not measured: it is the first call that runs the last functions'
  code, which pays for entering new code once (the `ISB` in `Jit::adopt`, a cold instruction cache),
  as synchronous call 5 does.
- **The six ladder benchmarks: no tail gain.** The p50s differ by at most 0.09 ms (`strcat`, 0.07%).
  On the five `‡` benchmarks nothing is compiled, so the compiler thread has only rejections to do
  (microseconds, Engine 4), and the p99 and max go both ways: higher in the background for `fib`,
  `strcat` and `closures`' p99, lower for `sieve`, `nbody` and `closures`' max. The largest
  differences are `strcat`'s (p99 138.60 against 143.03 ms, max 147.10 against 159.57 ms). These
  figures are single calls, and the slow ones are not where the JIT works: in `strcat` and `fib` the
  hot function is rejected during the first call, while the background's three slowest `strcat`
  calls are calls 5, 7 and 20, and its slowest `fib` call is call 18. For scale, `09_jit` and
  `jit_sync` run identical arguments in the same invocation, and their `strcat` maxima are 202.88 ms
  and 147.10 ms. So the differences on these five are within the spread of two identical
  configurations: no effect measured, in either direction.
- **`loop_sum`: the background's slowest calls were slower, and the compile is not why.** p50
  12.26 ms and 12.24 ms, but p99 31.71 ms against 43.15 ms and max 39.07 ms against 43.21 ms,
  and the median total per process is 264,492,208.5 ns against 275,402,978.5 ns (1.041×). All of
  it is the first call: calls 2 to 20 took 11,937,375 to 12,677,709 ns synchronously and
  11,922,625 to 13,252,625 ns in the background. The first call runs in the VM in both modes (the
  function is compiled during it, and there is no on-stack replacement), and the background
  compile happens on the other thread (16,333 to 23,333 ns per process there), so compiling in
  the background cannot shorten or lengthen that call by doing the compile. What differs is which
  group the first call falls in, the per-process grouping of rung 3e and Engine 4. Synchronously,
  8 of the 10 first calls took 31,076,125 to 31,709,625 ns, the others 36,968,042 and
  39,073,417; in the background, 8 took 42,587,000 to 43,206,708 ns, the others 36,466,458 and
  36,990,958. The slow group matches `08_fold`'s interpreted calls (p50 42.55 ms). With ten
  processes per configuration, the data cannot say whether `--jit-background` makes the slow
  group more likely or the runs fell that way. A hypothesis, not measured: rung 3e suspects that
  where a process's data lands decides its group, and the compiler thread, created at start-up,
  changes that (its stack, its allocations). Deciding it needs `loop_sum` run in many fresh
  processes with and without `--jit-background`, a measurement for the owner.
- **Result (D8 step 3).** On `bench/warmup.rg`, the benchmark built to make a compile pause
  visible (64 distinct functions, all compiled in the same short call), compiling in the
  background takes the pause off the engine's thread: its slowest calls are less than half as
  long (p99 0.82 to 0.32 ms, max 0.83 to 0.35 ms) and the total per process is lower, at the
  cost of slower 5th and 6th calls. On the six ladder benchmarks there is no tail gain: five
  compile nothing, and `loop_sum`'s one compile is too short to matter inside a first call that
  is interpreted either way; its worse tail in the background is not explained and cannot come
  from the compile itself. What this cannot show: that the effect matters outside a benchmark
  constructed for it (the pause it removes is about half a millisecond per process, once, here
  64 compiles of small functions); anything about another machine (one M1 Pro, D5); or effects
  smaller than the spread of a few slow calls among 200, which is what a p99 or a max is. The
  concurrency engineering (D8's handoff, TSan-checked in CI) stands on its own and does not
  depend on these numbers.
- **Expected versus measured.** On the six ladder benchmarks no change beyond noise was
  expected: it held for every p50 and for the p99 and max of the five benchmarks that compile
  nothing; `loop_sum`'s first-call grouping was not foreseen. On `warmup` the outcome was left
  open; measured, a gain on p99, max and total. The cost the expectation named, code arriving
  later and so more time in the interpreter, showed up in calls 5 and 6 rather than call 4 (which
  got much shorter), and it was smaller than the compiles moved off the engine's thread.

### JIT crashes and their causes

- **Baseline JIT (issue #23): no crash was hit while building it.** The direct tests of the
  generated code (`tests/unit/jit_test.cpp`) and the conformance suite passed under ASan + UBSan
  from their first run. The only failures were in the tests themselves: a wrong expected value
  in a new unit test, and a conformance test (`jit/loop_sum.rg`) first sized at 100,000
  iterations, which the tree-walker could not finish within the runner's timeout under ASan
  (it now loops 10,000 times).

- **Background compilation (issue #27): no crash, and no ThreadSanitizer report in the committed
  code.** The one surprise was a blind spot, not a bug: TSan on macOS did not see the `memcpy`
  that wrote the code, so a deliberately broken handoff went unreported until the copy, and the
  engine's check read of the first word, became plain 4-byte accesses (D8, "What
  ThreadSanitizer can and cannot see").

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
