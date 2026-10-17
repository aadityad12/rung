# Command-line reference

[Back to Rung](../README.md)

```text
rung [--engine=tree|stack|register|jit] program.rg
  --help, -h              show usage and available engines
  --version               show the version and engines in this build
  --stats                 heap statistics; instruction counts in debug builds
  --stats=pairs           which instruction follows which (register and stack VMs)
  --gc-stress             collect garbage on every allocation
  --fold                  constant folding and dead-code elimination
  --superinstructions     fused instruction pairs (register VM, JIT)
  --inline-cache          cached global lookups (register VM, JIT)
  --jit-threshold=N       calls plus loop iterations before compilation (default 1000)
  --jit-log               log compiles, rejections and bail-outs
  --jit-background        compile on a second thread
  --dump-tokens | --dump-ast | --dump-bytecode
  --bench=N --bench-out=FILE   time N calls of the program's run() function
```

The default engine is `tree`. The JIT needs an ARM64 machine and a NaN-boxed build. Flags that
an engine cannot use are rejected rather than silently ignored. Without an explicit engine,
`--dump-bytecode` shows stack bytecode. Use `--engine=register --dump-bytecode` for register
bytecode; disassembly does not execute the program.

Program output goes to stdout. Errors, statistics and JIT logs go to stderr. Exit codes are
0 for success (including help and version), 64 for invalid usage, 65 for compilation errors,
66 for unreadable input or benchmark output, and 70 for runtime errors. Programs must be regular
files; empty files succeed, and symlinks to regular files are accepted.

The counting tools need a Debug build, but no quiet machine: `scripts/instruction_counts.py`
checks per-opcode dispatch counts and `scripts/pair_counts.py` checks the pairs used to choose
superinstructions. `tests/run_jit_coverage_check.py` checks which benchmark functions actually
compile in a Release JIT build. These checks are part of CTest.

See [the demo](demo.md), [testing](testing.md), or [benchmark reproduction](performance.md#reproducing-the-numbers).
