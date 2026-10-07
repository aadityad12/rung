#!/usr/bin/env python3
"""Records which opcode pairs the register VM dispatches back to back, per benchmark (rung 3d).

This is what the choice of superinstructions rests on (docs/notes.md section 5, rung 3d). It
counts instructions, it does not time anything, so unlike scripts/bench.py it needs no exclusive
machine, but it does need a build with the VM counters (the `debug` preset).

    python3 scripts/pair_counts.py --rung build/debug/rung              # rewrite the file
    python3 scripts/pair_counts.py --rung build/debug/rung --check      # fail if it differs
    python3 scripts/pair_counts.py --rung build/debug/rung --superinstructions

Each benchmark runs as `rung --engine=register --stats=pairs --bench=1 bench/NAME.rg`: its top
level, then one call of run(). With --superinstructions the same is run with the fusions on,
which shows what is left after them (the output goes to docs/pair_counts_fused.txt).
"""
import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH_DIR = os.path.join(ROOT, "bench")


def pair_section(rung, name, superinstructions):
    cmd = [rung, "--engine=register", "--stats=pairs", "--bench=1"]
    if superinstructions:
        cmd.append("--superinstructions")
    with tempfile.TemporaryDirectory() as tmp:
        cmd += [f"--bench-out={os.path.join(tmp, 'out.json')}",
                os.path.join(BENCH_DIR, name + ".rg")]
        run = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if run.returncode != 0:
        sys.exit(f"{name}: rung exited {run.returncode}: {run.stderr}")
    lines = run.stderr.splitlines()
    instructions = next((l for l in lines if l.startswith("vm: ")), None)
    if instructions is None or "not compiled in" in instructions:
        sys.exit("this rung binary has no VM counters: use the debug preset")
    start = next(i for i, l in enumerate(lines) if l.startswith("pairs:"))
    return instructions, lines[start:]


def render(rung, superinstructions):
    flag = " --superinstructions" if superinstructions else ""
    out = [
        "Opcode pairs dispatched back to back by the register VM, per benchmark "
        "(counts, not times).",
        f"Produced by `python3 scripts/pair_counts.py{flag}` with a build that has the VM "
        "counters.",
        "Each run is `rung --engine=register --stats=pairs" + flag + " --bench=1 bench/NAME.rg`:",
        "the program's top level, then one call of run(). Only the 30 most frequent pairs of each",
        "benchmark are listed; the percentage is of all pairs dispatched in that run.",
        "",
    ]
    names = sorted(n[:-3] for n in os.listdir(BENCH_DIR) if n.endswith(".rg"))
    for name in names:
        instructions, section = pair_section(rung, name, superinstructions)
        out.append(f"== {name}")
        out.append(instructions)
        out.extend(section)
        out.append("")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rung", required=True, help="a rung built with RUNG_VM_COUNTERS=ON")
    parser.add_argument("--superinstructions", action="store_true",
                        help="count with the fusions on (writes docs/pair_counts_fused.txt)")
    parser.add_argument("--check", action="store_true", help="compare, do not write")
    args = parser.parse_args()
    target = os.path.join(
        ROOT, "docs", "pair_counts_fused.txt" if args.superinstructions else "pair_counts.txt")
    text = render(args.rung, args.superinstructions)
    if args.check:
        current = open(target).read() if os.path.exists(target) else ""
        if current != text:
            print(f"{target} differs from what this build produces", file=sys.stderr)
            return 1
        return 0
    with open(target, "w") as f:
        f.write(text)
    print(f"wrote {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
