#!/usr/bin/env python3
"""Checks bench/jit_not_compiled.json against what the JIT actually does (issue #24).

The README's results table marks a JIT row's cell with a double dagger when the JIT compiles no
function in that benchmark (scripts/ladder.py). This test keeps that mark honest: it runs every
benchmark the way scripts/bench.py runs the JIT rows (each row's arguments from
scripts/ladder_configs.json, bench.py's default iteration count, the default threshold) with
--jit-log, and fails if a listed benchmark compiles a function or an unlisted one compiles none.
It looks at which functions compile, never at timings (notes D5).

Rows with --jit-background are not run: which functions compile is decided by the same whitelist
on either thread, only *when* the code arrives differs, and a background compile still queued
when the process exits is cancelled, so its log would depend on scheduling.

It also checks the timing the warm-up benchmark (bench/warmup.rg, notes D8) is designed around:
with the JIT row's arguments, none of its functions is compiled in the first 3 calls of run(),
and every one except run() is by the end of the 4th.

    python3 tests/run_jit_coverage_check.py --rung build/release-goto-nanbox/rung
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import bench  # noqa: E402


def compiled_functions(rung, args, name, timeout, iterations=bench.DEFAULT_ITERATIONS):
    """The `[jit] compiled ...` and `[jit] rejected ...` lines of one benchmark process."""
    with tempfile.TemporaryDirectory() as tmp:
        cmd = [rung] + list(args) + ["--jit-log", f"--bench={iterations}",
                                     f"--bench-out={os.path.join(tmp, 'out.json')}",
                                     os.path.join(ROOT, "bench", name + ".rg")]
        run = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if run.returncode != 0:
        sys.exit(f"{name}: rung exited {run.returncode}: {run.stderr}")
    lines = run.stderr.splitlines()
    return ([l for l in lines if l.startswith("[jit] compiled ")],
            [l for l in lines if l.startswith("[jit] rejected ")])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rung", required=True, help="a rung built with the JIT")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    with open(os.path.join(ROOT, "bench", "jit_not_compiled.json"), encoding="utf-8") as f:
        not_compiled = json.load(f)
    benchmarks = list(bench.load_expected(ROOT))
    unknown = sorted(set(not_compiled) - set(benchmarks))
    failures = [f"bench/jit_not_compiled.json lists unknown benchmarks {unknown}"] if unknown \
        else []
    jit_rows = [c for c in bench.load_configs(ROOT)
                if "--engine=jit" in c["args"] and "--jit-background" not in c["args"]]
    if not jit_rows:
        failures.append("scripts/ladder_configs.json has no --engine=jit row")

    for config in jit_rows:
        for name in benchmarks:
            compiled, rejected = compiled_functions(args.rung, config["args"], name,
                                                    args.timeout)
            listed = name in not_compiled
            print(f"{config['id']} {name}: {len(compiled)} compiled, {len(rejected)} rejected"
                  f"{' (listed as not compiled)' if listed else ''}")
            for line in compiled + rejected:
                print("    " + line)
            if listed and compiled:
                failures.append(f"{config['id']} {name}: listed in bench/jit_not_compiled.json, "
                                "but the JIT compiled a function")
            if not listed and not compiled:
                failures.append(f"{config['id']} {name}: the JIT compiled nothing; add it to "
                                "bench/jit_not_compiled.json with the reason from --jit-log")

    if "warmup" in benchmarks:
        with open(os.path.join(ROOT, "bench", "warmup.rg"), encoding="utf-8") as f:
            hot = [line.split()[1].split("(")[0] for line in f if line.startswith("fn ")]
        hot.remove("run")
        for config in jit_rows:
            for calls, want in ((3, 0), (4, len(hot))):
                compiled, _ = compiled_functions(args.rung, config["args"], "warmup",
                                                 args.timeout, calls)
                print(f"{config['id']} warmup, {calls} calls of run(): {len(compiled)} compiled")
                if len(compiled) != want:
                    failures.append(f"{config['id']} warmup: {len(compiled)} functions compiled "
                                    f"in {calls} calls of run(), expected {want} (bench/warmup.rg "
                                    "is sized so all of them get hot in the 4th call)")

    for failure in failures:
        print("FAIL: " + failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
