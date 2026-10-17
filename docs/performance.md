# Performance: results and investigations

[Back to Rung](../README.md)

## Reading the results

The [generated tables](../README.md#results) retain every measured rung, including regressions.


How to read these tables:

- **Regressions stay in.** A cell below 1.00× in the marginal table is bold and marked ▼.
  NaN-boxing lost on all six benchmarks, and it stays in the ladder because everything above it
  builds on it: the JIT's type checks read NaN-box tags.
- **‡ means the JIT compiled nothing in that benchmark.** Every hot function there uses
  something the JIT doesn't support yet (global variables, which includes every call to a named
  function, float or string constants, closures, or built-in calls). Those cells measure the
  register VM, not machine code. The footnote under the tables says what each one uses.
- **† means the figure is uncertain.** `loop_sum` under `07_ic` was noisy in every attempt. Its
  speed falls into a few distinct groups from one process to the next, and I haven't found why.
- **Not every pair of rows was measured together.** The rows come from four benchmark sessions.
  Three of the marginal ratios (`02_stack` over `01_tree`, `07_ic` over `06_super`, and `09_jit`
  over `08_fold`) compare sessions taken at different times, so differences of a percent or two
  between them don't mean much.
- **Background compilation** helped only where a compile pause could matter. On
  `bench/warmup.rg`, a benchmark built so that 64 functions become hot in the same short call,
  compiling on a second thread cut the slowest calls by more than half, at the cost of slower
  5th and 6th calls while the compiled code wasn't ready yet. On the six main benchmarks a
  compile takes microseconds, so there was no pause to remove.

## One rung up close: stack VM to register VM

The register VM (`05_register` over `04_nanbox`) is the biggest single gain after leaving the
tree-walker on `sieve`, `nbody` and `closures`. It's also the rung the VMs' own instruction
counters can explain, so here it is on `sieve`.

**Instructions dispatched** for the script's top level plus one call of `run()`, counted by
the VMs themselves in a `debug` build. These are exact counts, not times. The per-opcode counts
are in [`docs/instruction_counts.txt`](instruction_counts.txt), which a test regenerates and
compares.

| `sieve`, one call | Stack VM | Register VM |
|---|---|---|
| Doing the work: `ADD MUL LE JUMP_IF_FALSE INDEX_GET INDEX_SET CALL CLOSURE GET_GLOBAL DEFINE_GLOBAL` | 8,051,972 | 8,051,972 |
| Moving values: `GET_LOCAL POP SET_LOCAL CONST FALSE TRUE NIL` | 19,949,723 | 0 |
| Loading a constant into a register: `LOADK LOADTRUE` | 0 | 5 |
| Jumping back or over: `LOOP JUMP` | 1,899,461 | 1,850,226 |
| Returning: `RETURN RETURN_NIL` | 2 | 2 |
| **Total** | **29,901,158** | **9,902,205** |

The useful work is identical, instruction for instruction. What disappears is moving values on
and off the stack. The inner loop (`flags[j] = false; j = j + i;` under `while (j <= n)`) shows
why. In stack bytecode one iteration is 16 instructions:

```
GET_LOCAL j; GET_LOCAL n; LE; JUMP_IF_FALSE; POP             // j <= n
GET_LOCAL flags; GET_LOCAL j; FALSE; INDEX_SET; POP          // flags[j] = false
GET_LOCAL j; GET_LOCAL i; ADD; SET_LOCAL j; POP              // j = j + i
LOOP
```

In register bytecode it's 5, because each instruction names its operands (`r0` is `n`, `r1` is
`flags`, `r2` is `i`, `r3` is `j`, and `k4` is the constant `false`):

```
LE r4 r3 r0; JUMP_IF_FALSE r4      // j <= n
INDEX_SET r1 r3 k4                 // flags[j] = false
ADD r3 r3 r2                       // j = j + i
JUMP
```

**Time.** In `results/04_nanbox.json` and `results/05_register.json` (measured in the same
session, interleaved, neither noisy) the median call went from 48.23 ms to 20.35 ms. A third of
the instructions took more than a third of the time, so each register instruction costs a bit
more than a stack one: it's a wider word to decode, and each operand can be a register or a
constant. I haven't measured exactly how much more, and I have no CPU-counter data (instructions
retired, branch misses) for any rung, because that needs a full Xcode install.

## What surprised me

I expected NaN-boxing to help, especially on `sieve`, where smaller values should make the large array cheaper to work with. It did cut values from 16 bytes to 8, and total heap allocation per process (20 calls) fell from about 192 MB to 96 MB (`results/03_goto.json` and `results/04_nanbox.json`). But every benchmark got slower. `sieve` had the biggest loss, at 0.63× the speed of the previous row. I don't know why yet: more expensive type checks, changes to the compiled dispatch loop, and a workload that wasn't limited by memory are all possible explanations, but none has been tested. Superinstructions gave me another result I hadn't expected: `strcat` dispatched 36.5% fewer instructions with almost no change in time. That suggests the string runtime is where most of its time goes, although I haven't confirmed that with a profile. Inline caching went the other way: `fib` gained 1.46×, more than I expected (`results/06_super.json` and `results/07_ic.json`; these two rows were measured in separate `bench.py` runs, so the ratio is not an interleaved comparison).

The JIT result needs some context. The 22.64× speedup over the tree-walker is for `loop_sum`, the only one of the six benchmarks where a function actually compiles. The others hit unsupported operations and keep running in the register VM. Even on `loop_sum`, the first call finishes in the interpreter because there is no on-stack replacement. Comparing the median call times gives a clearer view of the compiled calls: 42.55 ms before the JIT and 12.25 ms after (`results/08_fold.json` and `results/09_jit.json`, from separate runs). Compilation itself took about 16 µs, so moving it to another thread didn't produce a measurable tail-latency gain on the main suite. On `warmup`, a separate benchmark built to make 64 functions become hot together, background compilation reduced p99 from 0.82 ms to 0.32 ms (`results/jit_sync.json` and `results/jit_background.json`). Some later calls were slower because the code wasn't ready yet. I can point to a benefit on that test, but I can't generalize it to the whole suite. `loop_sum` also has an unresolved oddity: interpreted calls fall into distinct speed groups across processes, and its background tail was worse in this measurement. I don't have an explanation for that yet.

I thought generated machine code would be the most likely place for a crash. The baseline JIT tests passed from their first run; the intermittent crash around executable memory came from a sanitizer check on Linux ARM64. Before an indirect call, UBSan reads the eight bytes before the function's address. When the code started at the beginning of a mapped page, that read could reach unmapped memory. Leaving 16 zero bytes before the code fixed it. About 5% of runs crashed, so a single run usually passed; the cause was found by looping the test 300 to 600 times in CI (notes §5). The background compiler exposed a different problem with testing: on macOS, ThreadSanitizer missed a deliberately broken handoff while code was copied with `memcpy`. Plain four-byte stores, together with a C++ read of the code before execution, made the broken handoff detectable. That made me more careful about treating a clean sanitizer run as evidence: it matters whether the sanitizer can see the accesses being checked.

## Limitations and what I'd do next

- **The JIT covers a narrow slice of the language.** It compiles integer arithmetic,
  comparisons, jumps and array reads and writes. Calls, globals, floats, strings and closures
  stay in the VM, which is why it compiles one benchmark out of six. Compiling `fib` would need
  direct self-recursion. `sieve`'s loops already compile when they're handed the array, but its
  function also calls the built-in `array`, and calls are outside what the JIT supports.
- **No on-stack replacement.** A function that gets hot in the middle of a long call finishes
  that call in the interpreter, and only later calls run as machine code.
- **NaN-boxing's slowdown is unexplained.** My candidate explanations (costlier type checks, a
  differently compiled dispatch loop, a suite that was never waiting on memory) are all untested.
- **One machine, one suite.** Every number comes from one M1 Pro, and the superinstructions
  were chosen from this suite's own instruction-pair counts, so they are tuned to it.
- **The JIT is ARM64-only.** On x86-64, Rung runs on the tree-walker, stack VM and register VM.

## Reproducing the numbers

Every speed figure in this repository comes from one pipeline. `scripts/bench.py` measures the
configurations in `scripts/ladder_configs.json` and writes `results/<id>.json`, and
`scripts/ladder.py` turns those files into the tables above.

```sh
git status                      # must be clean: results are tied to a commit
python3 scripts/bench.py        # builds the presets and measures every configuration
python3 scripts/ladder.py       # regenerates the tables and the summary in this README
python3 scripts/ladder.py --check   # what CI runs
```

Run benchmarks with the machine plugged in, Low Power Mode off and nothing else running. Each
configuration and benchmark runs in 11 rounds of a fresh process that calls `run()` 20 times. In
every round the order is shuffled with a recorded seed, so background noise lands on all of them
alike. The first round is a warm-up and is discarded. A measurement whose spread (interquartile
range) is over 5% of its median is flagged as noisy. Every results file records the CPU, OS,
power source, compiler, git commit and date, plus peak memory, garbage-collector counters and
the JIT's compile time.

