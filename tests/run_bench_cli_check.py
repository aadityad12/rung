#!/usr/bin/env python3
"""Checks the --bench / --bench-out command line (issue #7, notes D12) with a tiny program, so it
runs in every preset, unlike the full-size benchmark check.

    python3 tests/run_bench_cli_check.py --rung build/debug/rung
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

PROGRAM = 'let calls = 0;\nfn run() { calls = calls + 1; return "n=" + "x"; }\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rung", required=True)
    args = parser.parse_args()
    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)

    with tempfile.TemporaryDirectory() as tmp:
        source = os.path.join(tmp, "p.rg")
        out = os.path.join(tmp, "out.json")
        with open(source, "w") as f:
            f.write(PROGRAM)

        def rung(*flags, path=source):
            return subprocess.run([args.rung, *flags, path], capture_output=True, text=True)

        run = rung("--engine=tree", "--bench=4", f"--bench-out={out}")
        check(run.returncode == 0 and not run.stdout, f"bench run failed: {run.returncode}")
        if run.returncode == 0:
            data = json.load(open(out))
            check(data["engine"] == "tree", "engine name missing")
            check(len(data["iterations_ns"]) == 4, "expected 4 iterations")
            check(all(isinstance(n, int) and n >= 0 for n in data["iterations_ns"]), "bad times")
            check(data["result"] == "n=x", f"result was {data['result']!r}")
            check("objects_allocated" in data["heap"], "heap stats missing")

        check(rung("--bench=3").returncode == 64, "--bench without --bench-out must be usage")
        check(rung(f"--bench-out={out}").returncode == 64, "--bench-out alone must be usage")
        for bad in ("--bench=0", "--bench=-1", "--bench=abc", "--bench="):
            check(rung(bad, f"--bench-out={out}").returncode == 64, f"{bad} must be usage")

        with open(source, "w") as f:
            f.write("fn run() { return 1 / 0; }\n")
        run = rung("--bench=2", f"--bench-out={out}")
        check(run.returncode == 70 and "division by zero" in run.stderr, "runtime error exit")

        with open(source, "w") as f:
            f.write("let x = 1;\n")
        run = rung("--bench=2", f"--bench-out={out}")
        check(run.returncode == 70, "missing run() must be a runtime error")

    for failure in failures:
        print("FAILURE:", failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
