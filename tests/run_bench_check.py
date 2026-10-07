#!/usr/bin/env python3
"""Runs every benchmark in bench/ once on one engine and checks its result (issue #7).

This checks correctness only, never timing: the numbers come from scripts/bench.py on the
development machine (notes D5). Every bench/*.rg must have an entry in bench/expected.json, and
every entry must have a file, so a benchmark cannot be added or dropped without a checksum.

    python3 tests/run_bench_check.py --rung build/release/rung --engine tree
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rung", required=True)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--inline-cache", action="store_true",
                        help="pass --inline-cache (register engine only)")
    parser.add_argument("--superinstructions", action="store_true",
                        help="pass --superinstructions (register engine only)")
    parser.add_argument("--jit-background", action="store_true",
                        help="pass --jit-background (jit engine only)")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    bench_dir = os.path.join(ROOT, "bench")
    with open(os.path.join(bench_dir, "expected.json")) as f:
        expected = json.load(f)
    names = sorted(n[:-3] for n in os.listdir(bench_dir) if n.endswith(".rg"))
    failures = []
    if names != sorted(expected):
        failures.append(f"bench/*.rg {names} does not match expected.json {sorted(expected)}")

    with tempfile.TemporaryDirectory() as tmp:
        for name in names:
            out = os.path.join(tmp, name + ".json")
            cmd = [args.rung, f"--engine={args.engine}"]
            if args.inline_cache:
                cmd.append("--inline-cache")
            if args.superinstructions:
                cmd.append("--superinstructions")
            if args.jit_background:
                cmd.append("--jit-background")
            cmd += ["--bench=1", f"--bench-out={out}", os.path.join(bench_dir, name + ".rg")]
            try:
                run = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
            except subprocess.TimeoutExpired:
                failures.append(f"{name}: timed out after {args.timeout}s")
                continue
            if run.returncode != 0 or run.stdout or run.stderr:
                failures.append(f"{name}: exit {run.returncode}, stdout {run.stdout!r}, "
                                f"stderr {run.stderr!r} (a benchmark must print nothing)")
                continue
            with open(out) as f:
                data = json.load(f)
            ok = (data["result"] == expected.get(name) and data["engine"] == args.engine
                  and len(data["iterations_ns"]) == 1)
            print(f"{'ok  ' if ok else 'FAIL'} {name}: {data['result']}")
            if not ok:
                failures.append(f"{name}: got {data!r}, expected result {expected.get(name)!r}")

    for failure in failures:
        print("FAILURE:", failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
