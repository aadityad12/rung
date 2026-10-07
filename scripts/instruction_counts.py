#!/usr/bin/env python3
"""Records how many instructions each VM dispatches per benchmark (issue #13, notes section 5).

The ladder table says how much faster each row is; these counts are the part of the "why" that
can be counted exactly. They count, they do not time anything, so unlike scripts/bench.py this
needs no exclusive machine, but it does need a build with the VM counters (the `debug` preset).

    python3 scripts/instruction_counts.py --rung build/debug/rung           # rewrite the file
    python3 scripts/instruction_counts.py --rung build/debug/rung --check   # fail if it differs

Each benchmark runs as `rung --engine=E [FLAGS] --stats --bench=1 bench/NAME.rg`: its top level,
then one call of run(), for the stack VM, the register VM, the register VM with
superinstructions, and the register VM with every rung. The output is
docs/instruction_counts.txt: a summary table, then each VM's per-opcode counts for each
benchmark.
"""
import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH_DIR = os.path.join(ROOT, "bench")
TARGET = os.path.join(ROOT, "docs", "instruction_counts.txt")

# (heading, rung arguments). The ladder rows these correspond to are in the file's header.
VMS = [("stack VM", ["--engine=stack"]),
       ("register VM", ["--engine=register"]),
       ("register VM + superinstructions", ["--engine=register", "--superinstructions"]),
       ("register VM + superinstructions + inline cache + folding",
        ["--engine=register", "--superinstructions", "--inline-cache", "--fold"])]


def vm_counts(rung, args, name):
    """Returns (instructions, calls, the `vm:` line and its per-opcode lines) for one run."""
    with tempfile.TemporaryDirectory() as tmp:
        cmd = [rung] + args + ["--stats", "--bench=1",
                               f"--bench-out={os.path.join(tmp, 'out.json')}",
                               os.path.join(BENCH_DIR, name + ".rg")]
        run = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if run.returncode != 0:
        sys.exit(f"{name}: rung exited {run.returncode}: {run.stderr}")
    lines = run.stderr.splitlines()
    start = next((i for i, l in enumerate(lines) if l.startswith("vm: ")), None)
    if start is None or "not compiled in" in lines[start]:
        sys.exit("this rung binary has no VM counters: use the debug preset")
    block = [lines[start]]
    for line in lines[start + 1:]:
        if not line.startswith("  "):
            break
        block.append(line)
    # "vm: N instructions dispatched, M calls, K native calls"
    words = lines[start].split()
    return int(words[1]), int(words[4]), block


def render(rung):
    names = sorted(n[:-3] for n in os.listdir(BENCH_DIR) if n.endswith(".rg"))
    counts = {name: [vm_counts(rung, args, name) for _, args in VMS] for name in names}
    out = [
        "Instructions dispatched per benchmark by the stack and register VMs (counts, not times).",
        "Produced by `python3 scripts/instruction_counts.py` with a build that has the VM "
        "counters.",
        "Each run is `rung --engine=E [FLAGS] --stats --bench=1 bench/NAME.rg`: the program's top",
        "level, then one call of run(). The counts do not depend on the dispatch style or the",
        "value representation, so the stack VM's are those of ladder rows 02 to 04, the register",
        "VM's those of row 05, `+ super` those of row 06 (and of row 07: the inline cache changes",
        "how a global is found, not how many instructions run), and `+ all` those of row 08.",
        "Row 09 runs the same bytecode, but the instructions of compiled functions run as",
        "machine code and are not counted (notes section 5, Engine 4).",
        "",
        "| Benchmark | Stack VM | Register VM | Register / stack | + super | + all | Calls |",
        "|---|---|---|---|---|---|---|",
    ]
    for name in names:
        (stack, stack_calls, _), (reg, reg_calls, _), (fused, _, _), (every, _, _) = \
            counts[name]
        calls = f"{stack_calls:,}" if stack_calls == reg_calls else \
            f"{stack_calls:,} / {reg_calls:,}"
        out.append(f"| {name} | {stack:,} | {reg:,} | {reg / stack:.2f} | {fused:,} | "
                   f"{every:,} | {calls} |")
    for name in names:
        out += ["", f"== {name}"]
        for (heading, _), (_, _, block) in zip(VMS, counts[name]):
            out.append(f"-- {heading}")
            out.extend(block)
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rung", required=True, help="a rung built with RUNG_VM_COUNTERS=ON")
    parser.add_argument("--check", action="store_true", help="compare, do not write")
    args = parser.parse_args()
    text = render(args.rung)
    if args.check:
        current = open(TARGET).read() if os.path.exists(TARGET) else ""
        if current != text:
            print(f"{TARGET} differs from what this build produces; rerun "
                  "scripts/instruction_counts.py and commit the result", file=sys.stderr)
            return 1
        return 0
    with open(TARGET, "w") as f:
        f.write(text)
    print(f"wrote {TARGET}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
