#!/usr/bin/env python3
"""Runs ladder configurations over the benchmarks and writes results/<config id>.json.

This is the only way a performance number gets into the repository (CLAUDE.md honesty rules):
scripts/ladder.py turns the files written here into the README table. Run it only on the
development Mac, plugged in, Low Power Mode off, other apps closed, machine untouched (notes D5).

    python3 scripts/bench.py                          # every config, every benchmark
    python3 scripts/bench.py --configs 01_tree --benchmarks fib sieve --runs 5

Standard library only. Python 3.9 or newer.
"""
import argparse
import json
import math
import os
import platform
import random
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone

DEFAULT_RUNS = 11  # rounds, the first of which is warm-up: 10 measured runs (notes D5)
DEFAULT_ITERATIONS = 20
DEFAULT_SEED = 1
NOISY_IQR_FRACTION = 0.05  # notes D5: flag a config whose IQR exceeds 5% of its median

SCRIPT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class BenchError(Exception):
    """Anything that must stop the measurement (a wrong result, a dirty tree, a failed build)."""


# ----------------------------------------------------------------------------- configuration


def load_configs(root):
    """Returns the ladder rows, in ladder order, from scripts/ladder_configs.json."""
    path = os.path.join(root, "scripts", "ladder_configs.json")
    with open(path, encoding="utf-8") as f:
        configs = json.load(f)
    if not isinstance(configs, list) or not configs:
        raise BenchError(f"{path}: expected a non-empty list of configurations")
    seen = set()
    for config in configs:
        for key in ("id", "label", "preset", "args"):
            if key not in config:
                raise BenchError(f"{path}: configuration {config!r} has no '{key}'")
        if config["id"] in seen:
            raise BenchError(f"{path}: duplicate configuration id '{config['id']}'")
        seen.add(config["id"])
    return configs


def load_expected(root):
    """Returns {benchmark name: expected result string}, in the file's order."""
    with open(os.path.join(root, "bench", "expected.json"), encoding="utf-8") as f:
        return json.load(f)


# ----------------------------------------------------------------------------- statistics


def median(values):
    return statistics.median(values)


def iqr(values):
    """Interquartile range (Q3 - Q1, inclusive method). A single run has no spread: 0."""
    if len(values) < 2:
        return 0
    q1, _, q3 = statistics.quantiles(values, n=4, method="inclusive")
    return q3 - q1


def percentile(values, p):
    """Nearest-rank percentile: always one of the observed samples, never an interpolation."""
    ordered = sorted(values)
    rank = max(1, -(-p * len(ordered) // 100))  # ceil(p * n / 100) in integers
    return ordered[rank - 1]


def summarize(runs, expected_result, iterations):
    """Turns the kept runs of one (config, benchmark) pair into the summary stored in JSON."""
    totals = [sum(run["iterations_ns"]) for run in runs]
    pooled = [ns for run in runs for ns in run["iterations_ns"]]
    med = median(totals)
    spread = iqr(totals)
    iqr_pct = (100.0 * spread / med) if med else 0.0
    return {
        "result": expected_result,
        "median_total_ns": med,
        "iqr_total_ns": spread,
        "iqr_pct": round(iqr_pct, 3),
        "noisy": bool(med and spread > NOISY_IQR_FRACTION * med),
        "p50_ns": percentile(pooled, 50),
        "p99_ns": percentile(pooled, 99),
        "max_ns": max(pooled),
        "wall_median_ns": median([run["wall_ns"] for run in runs]),
        "peak_rss_bytes_max": max(run["peak_rss_bytes"] for run in runs),
        "iterations": iterations,
        "heap": runs[-1].get("heap"),
        "runs": [{"total_ns": t, "wall_ns": r["wall_ns"], "peak_rss_bytes": r["peak_rss_bytes"],
                  "iterations_ns": r["iterations_ns"]} for t, r in zip(totals, runs)],
    }


# ----------------------------------------------------------------------------- environment


def capture(cmd, cwd=None):
    """Output of a command with surrounding whitespace removed, or None if it cannot run."""
    try:
        run = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return run.stdout.strip() if run.returncode == 0 else None


def cpu_model():
    brand = capture(["sysctl", "-n", "machdep.cpu.brand_string"])
    if brand:
        return brand
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as f:
            for line in f:
                if line.lower().startswith(("model name", "hardware", "cpu model")):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def os_version():
    if sys.platform == "darwin":
        version = capture(["sw_vers", "-productVersion"])
        if version:
            return "macOS " + version
    return platform.platform()


def power_source():
    """First line of `pmset -g batt`, e.g. "Now drawing from 'AC Power'"."""
    out = capture(["pmset", "-g", "batt"])
    return out.splitlines()[0] if out else "unknown"


def compiler_version(root, preset):
    """The compiler CMake used for this preset, with its version banner's first line."""
    compiler = "clang++"
    try:
        with open(os.path.join(root, "build", preset, "CMakeCache.txt"), encoding="utf-8") as f:
            for line in f:
                if line.startswith("CMAKE_CXX_COMPILER:"):
                    compiler = line.split("=", 1)[1].strip()
    except OSError:
        pass
    out = capture([compiler, "--version"])
    return out.splitlines()[0] if out else "unknown"


def git_state(root, allow_dirty):
    """Returns (commit, dirty). Refuses a dirty tree unless allowed: a result must map to a
    commit. results/ is ignored here because earlier invocations write into it."""
    commit = capture(["git", "rev-parse", "HEAD"], cwd=root)
    if commit is None:
        raise BenchError("not a git checkout with a commit; results must map to a commit")
    status = capture(["git", "status", "--porcelain", "--", ".", ":(exclude)results"], cwd=root)
    if status is None:
        raise BenchError("git status failed")
    dirty = bool(status)
    if dirty and not allow_dirty:
        raise BenchError("the git tree has uncommitted changes, so the results would not map to "
                         "a commit:\n" + status + "\ncommit them, or pass --allow-dirty "
                         "(recorded as dirty: true in the results)")
    return commit, dirty


def peak_rss_bytes(ru):
    """ru_maxrss is bytes on macOS and kilobytes on Linux."""
    return ru.ru_maxrss if sys.platform == "darwin" else ru.ru_maxrss * 1024


# ----------------------------------------------------------------------------- running


def build_presets(root, presets):
    for preset in presets:
        for cmd in (["cmake", "--preset", preset], ["cmake", "--build", "--preset", preset]):
            print("+ " + " ".join(cmd), flush=True)
            if subprocess.run(cmd, cwd=root).returncode != 0:
                raise BenchError(f"'{' '.join(cmd)}' failed")


def run_process(root, preset, args, benchmark, iterations):
    """One `rung ARGS --bench=N --bench-out=TMP bench/X.rg` process. Returns its measurements.

    The process is waited on with os.wait4 so the kernel's rusage (peak RSS) comes back with
    its exit status; subprocess.run would discard it."""
    binary = os.path.join(root, "build", preset, "rung")
    source = os.path.join(root, "bench", benchmark + ".rg")
    with tempfile.TemporaryDirectory() as tmp:
        out_path = os.path.join(tmp, "out.json")
        cmd = [binary] + list(args) + [f"--bench={iterations}", f"--bench-out={out_path}", source]
        with open(os.path.join(tmp, "stdout"), "w+b") as out, \
                open(os.path.join(tmp, "stderr"), "w+b") as err:
            start = time.perf_counter_ns()
            try:
                proc = subprocess.Popen(cmd, stdout=out, stderr=err, cwd=root)
            except OSError as e:
                raise BenchError(f"cannot run {binary}: {e}")
            _, status, ru = os.wait4(proc.pid, 0)
            wall_ns = time.perf_counter_ns() - start
            proc.returncode = os.waitstatus_to_exitcode(status)
            out.seek(0)
            err.seek(0)
            stdout, stderr = out.read().decode(errors="replace"), err.read().decode(
                errors="replace")
        if proc.returncode != 0 or stdout or stderr:
            raise BenchError(f"{' '.join(cmd)}: exit {proc.returncode}, stdout {stdout!r}, "
                             f"stderr {stderr!r} (a benchmark must exit 0 and print nothing)")
        with open(out_path, encoding="utf-8") as f:
            data = json.load(f)
    return {"iterations_ns": data["iterations_ns"], "result": data["result"], "wall_ns": wall_ns,
            "peak_rss_bytes": peak_rss_bytes(ru), "heap": data.get("heap")}


def measure(root, configs, benchmarks, expected, runs, iterations, seed, log=print):
    """Interleaved measurement. Returns {config id: {benchmark: [kept runs]}}.

    Each round runs every (config, benchmark) pair once, in an order shuffled by `seed`, so a
    burst of background activity lands on every configuration instead of one. Round 1 is
    warm-up (caches, page faults, frequency ramp-up): it is run and checked, then discarded."""
    pairs = [(c, b) for c in configs for b in benchmarks]
    rng = random.Random(seed)
    kept = {c["id"]: {b: [] for b in benchmarks} for c in configs}
    for round_number in range(1, runs + 1):
        order = list(pairs)
        rng.shuffle(order)
        label = "warm-up, discarded" if round_number == 1 else "measured"
        log(f"round {round_number}/{runs} ({label})", flush=True)
        for config, benchmark in order:
            run = run_process(root, config["preset"], config["args"], benchmark, iterations)
            if run["result"] != expected[benchmark]:
                raise BenchError(f"{config['id']} {benchmark}: result {run['result']!r}, "
                                 f"expected {expected[benchmark]!r} (bench/expected.json)")
            if len(run["iterations_ns"]) != iterations:
                raise BenchError(f"{config['id']} {benchmark}: {len(run['iterations_ns'])} "
                                 f"iteration times, expected {iterations}")
            if round_number > 1:
                kept[config["id"]][benchmark].append(run)
    return kept


# ----------------------------------------------------------------------------- output


def describe_changes(old, new):
    """Lines saying how a results file differs from the one it replaces."""
    lines = []
    old_meta, new_meta = old.get("meta", {}), new["meta"]
    if old_meta.get("commit") != new_meta["commit"]:
        lines.append(f"  commit {str(old_meta.get('commit'))[:10]} -> {new_meta['commit'][:10]}")
    for name, entry in new["benchmarks"].items():
        before = old.get("benchmarks", {}).get(name)
        after = entry["median_total_ns"] / entry["iterations"]
        if before is None:
            lines.append(f"  {name}: new, {after / 1e6:.3f} ms per iteration")
            continue
        was = before["median_total_ns"] / before["iterations"]
        change = 100.0 * (after - was) / was if was else 0.0
        lines.append(f"  {name}: {was / 1e6:.3f} ms -> {after / 1e6:.3f} ms per iteration "
                     f"({change:+.1f}%)")
    for name in old.get("benchmarks", {}):
        if name not in new["benchmarks"]:
            lines.append(f"  {name}: DROPPED (this run did not include it)")
    return lines


def write_results(root, config, meta, summaries, log=print):
    results_dir = os.path.join(root, "results")
    os.makedirs(results_dir, exist_ok=True)
    path = os.path.join(results_dir, config["id"] + ".json")
    new = {"config": config, "meta": meta, "benchmarks": summaries}
    if os.path.exists(path):
        try:
            with open(path, encoding="utf-8") as f:
                old = json.load(f)
        except (OSError, ValueError):
            old = {}
        log(f"replacing {os.path.relpath(path, root)}; what changed:")
        for line in describe_changes(old, new):
            log(line)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(new, f, indent=2)
        f.write("\n")
    return path


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--configs", nargs="+", metavar="ID",
                        help="ladder configuration ids (default: all)")
    parser.add_argument("--benchmarks", nargs="+", metavar="NAME",
                        help="benchmark names (default: all in bench/expected.json)")
    parser.add_argument("--runs", type=int, default=DEFAULT_RUNS,
                        help="rounds per pair, including the discarded warm-up round "
                             f"(default {DEFAULT_RUNS}: 10 measured)")
    parser.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS,
                        help=f"calls of run() per process (default {DEFAULT_ITERATIONS})")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED,
                        help="seed for the per-round shuffle; recorded in the results")
    parser.add_argument("--allow-dirty", action="store_true",
                        help="measure even with uncommitted changes (recorded as dirty)")
    parser.add_argument("--no-build", action="store_true",
                        help="use the binaries already in build/<preset>/")
    parser.add_argument("--root", default=SCRIPT_ROOT, help=argparse.SUPPRESS)  # for tests
    args = parser.parse_args(argv)
    if args.runs < 2:
        parser.error("--runs must be at least 2 (one warm-up round plus one measured)")
    if args.iterations < 1:
        parser.error("--iterations must be at least 1")
    return args


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    root = args.root
    try:
        all_configs = load_configs(root)
        expected = load_expected(root)
        by_id = {c["id"]: c for c in all_configs}
        for wanted, known, what in ((args.configs, by_id, "configuration"),
                                    (args.benchmarks, expected, "benchmark")):
            for name in wanted or []:
                if name not in known:
                    raise BenchError(f"unknown {what} '{name}'; known: {', '.join(known)}")
        configs = [by_id[i] for i in args.configs] if args.configs else all_configs
        benchmarks = list(args.benchmarks) if args.benchmarks else list(expected)

        commit, dirty = git_state(root, args.allow_dirty)
        if not args.no_build:
            build_presets(root, sorted({c["preset"] for c in configs}))

        started = datetime.now(timezone.utc)
        kept = measure(root, configs, benchmarks, expected, args.runs, args.iterations, args.seed)

        for config in configs:
            summaries = {b: summarize(kept[config["id"]][b], expected[b], args.iterations)
                         for b in benchmarks}
            meta = {
                "cpu": cpu_model(),
                "os": os_version(),
                "power_source": power_source(),
                "compiler": compiler_version(root, config["preset"]),
                "commit": commit,
                "dirty": dirty,
                "date": started.strftime("%Y-%m-%dT%H:%M:%SZ"),
                "runs": args.runs,
                "kept_runs": args.runs - 1,
                "iterations": args.iterations,
                "seed": args.seed,
            }
            path = write_results(root, config, meta, summaries)
            print("wrote " + os.path.relpath(path, root))
            for name, entry in summaries.items():
                if entry["noisy"]:
                    print(f"WARNING: {config['id']} {name} is noisy (IQR {entry['iqr_pct']:.1f}% "
                          f"of the median, limit {100 * NOISY_IQR_FRACTION:.0f}%); close other "
                          f"apps and rerun: bench.py --configs {config['id']}", file=sys.stderr)
    except BenchError as e:
        print("bench.py: " + str(e), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
