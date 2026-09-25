# Conformance tests

Small Rung programs that say, in comments, what they must do. `tests/run_conformance.py` runs any
engine over all of them. Every engine must pass every test with identical output: this suite is
the executable form of the semantics contract in [`docs/notes.md`](../../docs/notes.md) §2, and
the format is decided in notes D13.

## Running

```sh
python3 tests/run_conformance.py --rung build/debug/rung --engine tree
python3 tests/run_conformance.py --rung build/asan/rung --engine tree --gc-stress
python3 tests/run_conformance.py --rung build/debug/rung --engine tree numbers/ closures/
```

| Option | Meaning |
|---|---|
| `--rung PATH` | the `rung` executable (required) |
| `--engine NAME` | passed to rung as `--engine=NAME` (required) |
| `--gc-stress` | passed to rung as `--gc-stress` (collect on every allocation) |
| `--timeout SEC` | per-test limit, default 10. A test over the limit fails, so an infinite loop cannot hang CI. `--gc-stress` under a sanitizer is much slower, and the 10,000-deep recursion tests are the slowest, so raise this there. |
| `--jobs N` | tests run in parallel, default the CPU count |
| `--skip SUBSTRING` | do not run tests whose path contains it (repeatable). Skipped tests are listed as `SKIPPED` and counted in the last line, so nothing is skipped silently. CMake uses it only for the deep-recursion tests under `--gc-stress` in the ASan preset (see `CMakeLists.txt`). |
| `FILTER...` | run only tests whose path (relative to this directory) contains one of these strings |

Each failing test prints one `FAIL path` line, then what differed (exit code, a small diff of
stdout, the first line of stderr). The last line is `N passed, M failed`. The exit status is 1
if anything failed, or if no test matched the filters.

Python 3, standard library only.

## Writing a test

A test is `tests/conformance/<topic>/<what_it_checks>.rg`. Name the file after the behaviour
(`int_division_truncates_toward_zero.rg`), keep it small, and check one rule. Put the expected
result in comments, on the line of code that produces it:

```
print 1 + 2 * 3; // expect: 7
```

| Annotation | Meaning |
|---|---|
| `// expect: TEXT` | one line of stdout. All `expect:` lines, in file order, must equal stdout exactly. One space after the colon is syntax; the rest is the text, so `// expect:` alone is an empty line. |
| `// expect runtime error: MSG` | stderr's first line is `[line L] runtime error: MSG`, exit code 70. `L` is the line the comment is on. |
| `// expect compile error: MSG` | the same with `compile error`, exit code 65. |
| (no error annotation) | exit code 0 and empty stderr |

The runner checks stdout, the exit code and the first line of stderr, exactly. At most one error
annotation per file.

Rules that keep tests honest:

- **Put an error's comment on the line the error is reported on.** For a runtime error that is
  the operator, the call's `(`, the index's `[`, or the variable's name (notes §2.5). When a
  statement spans lines, the comment goes on the line of that token, not the first line of the
  statement (see `errors/line_is_the_operator_line.rg`).
- **Output can precede a runtime error.** `expect:` lines before it are still checked. A compile
  error prints nothing to stdout, because nothing runs.
- **Errors at the end of the input** (for example a missing `}`) are reported on the line of the
  end-of-file token. Such a test ends without a trailing newline, with the annotation on its last
  line.
- **A program that prints several lines** from one statement can list them as comment-only
  `// expect:` lines after it. They count in file order.
- **Never print `clock()`**, and never depend on something the notes leave unspecified (for
  example, the order in which the two sides of an index assignment are checked when both are
  wrong). If a test needs a rule, the rule belongs in `docs/notes.md` first.
- **Don't write `// expect` inside a string literal.** The runner reads annotations by text, not by
  lexing. (The unterminated-string tests do it on purpose: the comment is inside the string.)
- Nothing may depend on when the GC runs. Every test also runs with `--gc-stress`.

## Topics

| Directory | Covers |
|---|---|
| `lexing/` | every lexer error, number and string forms, comments, keywords vs identifiers |
| `parsing/` | every parser error message (§2.7), precedence and associativity, `-2147483648`, the nesting limit |
| `resolving/` | every resolver error, shadowing, the D11 example, the 255/256 limits |
| `numbers/` | every row of §2.1: wraparound, division, modulo, float mixing, IEEE cases |
| `types/` | every row of §2.2: truthiness, `and`/`or`, string and identity equality |
| `printing/` | every row of §2.3 and the float algorithm |
| `errors/` | every runtime error message (§2.5), reported lines, exit codes |
| `control_flow/` | `if`/`else`, `while`, every form of `for`, short-circuiting |
| `functions/` | recursion, arity, functions as values, the 10,000-frame limit |
| `closures/` | captured variables, shared `for` variable (§2.6), closures in closures |
| `arrays/` | literals, `array(n, fill)`, aliasing, nesting, `len` |
| `strings/` | concatenation, equality, escapes, multi-line strings |
| `globals/` | redeclaration, undefined reads and assignments |
| `programs/` | a few small whole programs (sieve, sort, linked list) that stress the collector |

A few tests are generated (a 255-parameter function, 200 nested blocks) because writing them by
hand is error-prone. The generated files are ordinary tests and are edited like any other.

## The suite pins some awkward corners on purpose

`printing/float_with_trailing_zeros_prints_in_exponent_form.rg` records that `100.0` prints as
`1e+02`. That is what the float algorithm in notes §2.3 produces (`%.1g` gives `1e+02`, which
reads back as exactly 100). If the algorithm is ever changed, change the notes first, then this
test.
