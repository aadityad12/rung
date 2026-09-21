# Working on Rung

Rung is a small dynamically typed language with several execution engines (tree-walker, stack
VM, register VM with an optimization ladder, ARM64 JIT, background JIT compilation), built to
measure each speedup honestly. Read `README.md` for the overview.

## Before you write code

1. Read the GitHub issue you were given in full, including every issue it says it depends on.
2. Read `docs/notes.md`. It is the **source of truth** for design decisions (§1) and for the
   semantics every engine must follow exactly (§2). If the issue and `docs/notes.md` disagree,
   `docs/notes.md` wins. Stop and say so in the PR rather than guessing.
3. Don't change a `DECIDED` entry on your own. If one seems wrong, finish what you can and
   explain the problem in the PR description.

## Build and test

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan    # before every PR
```
- Presets: `debug`, `release` (benchmarks only), `asan` (ASan + UBSan), `tsan`.
- Warnings are errors (`-Werror`) on our code. Fix warnings; don't silence them. Never add
  `-Wno-...` flags or pragmas without a comment explaining why.
- CI runs macOS ARM64, Linux ARM64 and Linux x86-64 (Docker). The JIT only exists on ARM64.
- Apple's libc++ on the CI Mac (Xcode 16) is older than a current Mac's: no floating-point
  `std::from_chars`, for example. Prefer long-established standard library features.

## Code conventions

- C++20, clang. No dependencies beyond the standard library and vendored `third_party/`.
  Never add LLVM, asmjit, or any code generation library: emitting machine code by hand is the
  point of the JIT.
- Match the style of the existing code: 4-space indent, 100-column lines, `snake_case`
  functions and variables, `PascalCase` types, `kConstant` constants, trailing `_` on private
  members, everything in `namespace rung`.
- Comments explain *why*, not *what*. Cite the `docs/notes.md` entry (e.g. `// notes D11`)
  when code exists because of a decision.
- Every engine gets its semantics from `src/runtime/` (notes D10). Never reimplement an
  arithmetic, comparison, printing, or error-message rule inside an engine.
- Signed integer overflow is undefined behaviour in C++; Rung ints wrap (notes D1). Do wrapping
  math in `uint32_t`.
- Every new behaviour gets tests: doctest unit tests in `tests/unit/`, and conformance tests in
  `tests/conformance/` (format in notes D13) for anything a Rung program can observe.

## Honesty rules (non-negotiable)

- **No invented numbers.** Never write a performance figure anywhere (README, notes, PR, code
  comments) unless it comes from a committed `results/*.json` produced by `scripts/bench.py`.
- **Never hand-edit the results table** in the README. `scripts/ladder.py` generates it.
- **Report regressions and small gains as they are.** A rung that gains 2% or loses 5% is
  reported as exactly that, with an explanation, in `docs/notes.md` §5.
- Keep the *Crafting Interpreters* and Lua 5.0 acknowledgements in the README accurate. If you
  follow either closely, say so in a comment.
- Numbers describe output, never effort: no commit counts or line counts anywhere.

## Pull requests

- One issue per PR, branch named `<issue-number>-<short-slug>`, PR body says `Closes #N`.
- CI must be green on all jobs. Run the `asan` preset locally first.
- Fill in the PR template completely. The **How it works** and **Be ready to answer** sections
  are required: the project owner learns the codebase from them and has to be able to defend
  every design choice in an interview. Write them for a strong CS student who has not seen this
  code: plain language, concrete examples, the *why* behind each decision.
- Commit messages: imperative summary line, body explaining why. No schedule or week labels
  anywhere in the repo, in commits, or in PRs.
- **Never merge your own PR.** The `/autopilot` orchestrator merges, following the rules in the
  Autopilot section below.
- **Never** install software, run `scripts/bench.py`, or change a `DECIDED` entry without the
  owner. If an issue needs any of that, finish everything else, push, and say exactly what's
  needed and why.

## Autopilot

Configuration and project rules for the `/autopilot` skill (installed globally from
github.com/aadityad12/claude-skills). Subagents follow everything above; the orchestrator also
follows these.

- max-parallel: 3
- owner-label: needs-owner
- careful-label: hard
- skip-labels: stretch, tracking
- merge-method: squash
- worker-model: sonnet
- careful-model: opus
- **Benchmarks are an exclusive task.** Issues whose steps say "Measure (ask owner)" or run
  `scripts/bench.py`: the subagent implements everything else and opens the PR; the measurement
  waits for the owner. Ask him to plug the Mac in, turn Low Power Mode off, close other apps, and
  not use the machine until you report back (notes D5), with a time estimate. When he says "go",
  stop all subagents, run `scripts/bench.py` on the PR branch in a clean worktree, rerun anything
  flagged noisy, run `scripts/ladder.py`, commit the results to the same PR, and tell him the
  Mac is free again.
- `needs-owner` issues that are only benchmark runs (e.g. recording a ladder row) need no
  subagent: they are exclusive tasks from the start.
- The owner learns the codebase from each PR's **How it works** and **Be ready to answer**
  sections. In the stop report, point him at those when asking for a review.

