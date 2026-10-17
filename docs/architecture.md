# How Rung works

[Back to Rung](../README.md)

## The language

Deliberately tiny, because every feature has to be built once per engine: 32-bit integers that
wrap around, 64-bit floats, booleans, `nil`, immutable strings, fixed-size arrays, first-class
functions with closures, `let`, `if`, `while`, `for`, `return` and `print`, plus three built-in
functions, `array`, `len` and `clock`.

```
fn make_counter() {
  let count = 0;
  fn next() { count = count + 1; return count; }
  return next;
}
let counter = make_counter();
counter();
print counter();   // 2
```

The exact rules every engine has to agree on (integer wraparound, division by zero, how floats
print, every error message and exit code) are written down in [`docs/notes.md`](notes.md)
§2.

## How it works

```
  source.rg -> lexer -> parser -> resolver -> [fold]      front end, the same for every engine
                                                 |
          +--------------------------------------+---------------------+
          |                       |                                    |
          v                       v                                    v
     tree-walker           stack compiler                      register compiler
    --engine=tree                 |                                    |
                                  v                           [superinstructions]
                              stack VM                                 |
                           --engine=stack                              v
                                                                 register VM
                                                     --engine=register, [inline cache]
                                                            |                ^
                                             hot function   |                |  bail-out: resume at
                                             (whitelisted)  v                |  the same bytecode
                                                       [baseline JIT] -------+  instruction
                                                   ARM64 code, --engine=jit
                                         (with --jit-background, compiled on a second thread)

  Every engine calls the shared runtime, src/runtime/: the Value type (tagged struct or
  [NaN-boxed]), arithmetic, comparison, printing and error messages, the heap and its
  mark-sweep garbage collector, interned strings.

  [brackets]: one ladder rung, switchable on its own. Computed-goto dispatch and NaN-boxed values
  are build options (CMake presets) shared by both VMs; the other rungs are command-line flags.
```

1. **The front end** is shared by every engine. The lexer turns text into tokens, the parser
   builds a syntax tree, and the resolver works out where every variable lives (a local slot, a
   captured variable, or a global) and reports errors like using a variable before it's
   declared. `--fold` can simplify the tree first.
2. **The engines.** The tree-walker runs the syntax tree directly. The two compilers turn it into
   bytecode: one for the stack VM, one for the register VM.
3. **The JIT** isn't a separate engine. It's the register VM plus a code generator. When a
   function gets hot and only uses operations the JIT supports, it's compiled to ARM64. The
   machine code reads and writes the same register slots the VM uses, so if one of its type
   checks fails (say, a float turns up where it assumed an integer), it simply returns the
   bytecode position to resume at and the VM carries on from there.
4. **The shared runtime** in `src/runtime/` gives every engine the same values, arithmetic,
   printing, error messages and garbage collector. That's what makes the comparison fair: a rung
   can only change how fast a program runs, never what it does.

### The ladder

| Rung | What it changes | The idea |
|---|---|---|
| `01_tree` | Tree-walking interpreter | Run the syntax tree directly. The baseline. |
| `02_stack` | Stack-based bytecode VM | Compile to compact bytecode and run it in a tight loop. |
| `03_goto` | Computed-goto dispatch | Jump straight from one instruction's handler to the next, instead of going back through one `switch`. |
| `04_nanbox` | NaN-boxed values | Pack every value into 8 bytes by hiding integers and pointers inside unused floating-point "not a number" patterns. |
| `05_register` | Register-based VM | Instructions name their operands directly, so far fewer instructions only move values around. |
| `06_super` | Superinstructions | Fuse the most common instruction pairs into one. |
| `07_ic` | Inline caching for globals | Remember where each global variable lives instead of looking it up by name every time. |
| `08_fold` | Constant folding | Compute things like `2 * 3` once, before the program runs. |
| `09_jit` | Baseline JIT | Compile hot functions to ARM64 machine code. |

Each rung keeps everything below it, so row 09 is the register VM with every optimization on
plus the JIT. A tenth configuration, the background compiler thread, is measured separately.

## Project layout

```
src/                  the language: front end, engines, compilers
  runtime/            values, garbage collector, arithmetic and errors shared by every engine
  bytecode/           stack and register instruction formats
  vm/                 dispatch macros and instruction counters shared by both VMs
  jit/                ARM64 encoder, code generator, executable memory, compiler thread
tests/conformance/    Rung programs with expected output, run on every engine
tests/unit/           C++ unit tests
bench/                the benchmark programs and their expected checksums
results/              committed benchmark results (one file per configuration)
scripts/              benchmarking and table generation
docs/notes.md         every design decision and what each rung measured
fuzz/                 the libFuzzer target
```

## Design notes

[`docs/notes.md`](notes.md) is the record of every decision behind this project: what I
chose, what I replaced, and why (§1). It has the exact semantics every engine must follow (§2),
and, for every rung, what I expected, what I measured, and why the two differed, including every
crash the JIT work turned up (§5).

