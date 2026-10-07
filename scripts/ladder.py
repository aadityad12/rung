#!/usr/bin/env python3
"""Generates the README results tables from results/*.json (written by scripts/bench.py).

    python3 scripts/ladder.py            # rewrite the text between the markers in README.md
    python3 scripts/ladder.py --check    # exit 1 if README.md differs from what would be written

The README has two generated blocks: the tables under "Results" (between the `ladder:` markers)
and the one-paragraph summary near the top (between the `summary:` markers). Neither is ever
edited by hand. `--check` runs in CI, so a hand-edited (or stale) block fails the build: it
regenerates both from the committed results files and compares them with the README byte for
byte.

Configurations marked `"ladder": false` in scripts/ladder_configs.json are not ladder rows (notes
D4 fixes the ladder at nine): they are measured by bench.py like any other, but appear only in
their own table, the background-compilation comparison (notes D8), never in the speedup tables
or the summary.

Standard library only.
"""
import argparse
import difflib
import json
import math
import os
import sys

from bench import BenchError, load_configs, load_expected

START_MARKER = "<!-- ladder:start -->"
END_MARKER = "<!-- ladder:end -->"
SUMMARY_START_MARKER = "<!-- summary:start -->"
SUMMARY_END_MARKER = "<!-- summary:end -->"
SCRIPT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

NOISY_MARK = "†"      # dagger
REGRESSION_MARK = "▼"  # down-pointing triangle
NOT_JIT_MARK = "‡"    # double dagger
TIMES = "×"           # multiplication sign


class LadderError(Exception):
    pass


# ----------------------------------------------------------------------------- loading


def load_results(root, configs):
    """Returns {config id: parsed results file} for every config that has one."""
    results = {}
    for config in configs:
        path = os.path.join(root, "results", config["id"] + ".json")
        if not os.path.exists(path):
            continue
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            if data["config"]["id"] != config["id"]:
                raise LadderError(f"{path}: config id {data['config']['id']!r} does not match "
                                  "its file name")
            data["meta"]["commit"], data["benchmarks"]  # must exist
        except (OSError, ValueError, KeyError, TypeError) as e:
            raise LadderError(f"{path}: not a results file written by bench.py ({e!r})")
        results[config["id"]] = data
    return results


def load_not_jit_compiled(root):
    """Returns {benchmark: why}, from bench/jit_not_compiled.json, for the benchmarks in which
    the JIT compiles no function at its default threshold. Absent file: none are listed.

    The file is checked against `rung --jit-log` by a ctest (tests/run_jit_coverage_check.py),
    so a mark in the table cannot claim something the JIT no longer does."""
    path = os.path.join(root, "bench", "jit_not_compiled.json")
    if not os.path.exists(path):
        return {}
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, dict) or not all(isinstance(v, str) for v in data.values()):
            raise ValueError("expected an object of strings")
    except (OSError, ValueError) as e:
        raise LadderError(f"{path}: {e}")
    return data


def is_jit_config(config):
    return "--engine=jit" in config["args"]


def is_ladder_row(config):
    """A row of the cumulative ladder (notes D4), as opposed to an extra comparison."""
    return config.get("ladder", True)


def measured_benchmarks(results, configs, benchmarks):
    """The benchmarks, in bench/expected.json order, that at least one of `configs` measured.
    A benchmark added after the ladder rows were recorded (bench/warmup.rg, notes D8) gets a
    column once a ladder row measures it, instead of a column of "n/a" in every row."""
    return [b for b in benchmarks
            if any(b in results[c["id"]]["benchmarks"] for c in configs if c["id"] in results)]


# ----------------------------------------------------------------------------- rendering


def cell_entry(results, config_id, benchmark):
    """The summary for one (config, benchmark), or None if it was not measured."""
    data = results.get(config_id)
    return data["benchmarks"].get(benchmark) if data else None


def per_iteration_ns(entry):
    # Normalised by iteration count so results taken with different --iterations compare.
    return entry["median_total_ns"] / entry["iterations"]


def ratio_cell(numerator, denominator):
    """Both arguments are summary entries (or None). The result is how many times faster the
    `denominator` entry is than the `numerator` one: the reference's time over the row's."""
    if numerator is None or denominator is None:
        return "n/a"
    ratio = per_iteration_ns(numerator) / per_iteration_ns(denominator)
    text = f"{ratio:.2f}{TIMES}"
    if ratio < 1.0:
        text = f"**{text} {REGRESSION_MARK}**"
    if numerator["noisy"] or denominator["noisy"]:
        text += " " + NOISY_MARK
    return text


def significant(x, digits=2):
    """`x` to `digits` significant figures, never in exponent notation: a share of 0.00019%
    must not print as 0.000% (which reads as "nothing") or as 1.9e-04%."""
    if x <= 0:
        return "0"
    decimals = max(0, digits - 1 - math.floor(math.log10(x)))
    return f"{x:.{decimals}f}"


def table(header, rows):
    lines = ["| " + " | ".join(header) + " |", "|" + "|".join(" --- " for _ in header) + "|"]
    lines += ["| " + " | ".join(cells) + " |" for cells in rows]
    return "\n".join(lines)


def row_name(config):
    return f"`{config['id']}` {config['label']}".replace("|", "\\|")


def machine_text(meta):
    return f"{meta['cpu']}, {meta['os']}".replace("|", "\\|")


def render(root):
    """The generated README text (without the markers)."""
    all_configs = load_configs(root)
    configs = [c for c in all_configs if is_ladder_row(c)]
    extras = [c for c in all_configs if not is_ladder_row(c)]
    results = load_results(root, all_configs)
    benchmarks = measured_benchmarks(results, configs, list(load_expected(root)))
    not_jit = load_not_jit_compiled(root)
    base = configs[0]
    out = ["_This section is generated by `scripts/ladder.py` from the files in `results/`. "
           "Do not edit it by hand: `ladder.py --check` runs in CI and fails if it differs._"]

    if not any(c["id"] in results for c in configs):
        out += ["", "No results have been recorded yet. Each row appears here once "
                "`scripts/bench.py` has measured it on the development machine (notes D5)."]
        return "\n".join(out)

    def jit_mark(config, benchmark):
        # A JIT row's cell for a benchmark the JIT compiles nothing in measures the register VM
        # plus the JIT's bookkeeping, not machine code (notes D7), so it says so.
        if is_jit_config(config) and benchmark in not_jit and config["id"] in results:
            return " " + NOT_JIT_MARK
        return ""

    # Table 1: cumulative speedup over the first row.
    rows = []
    for config in configs:
        cells = [row_name(config)]
        for b in benchmarks:
            if config["id"] not in results:
                cells.append("not measured")
            else:
                cells.append(ratio_cell(cell_entry(results, base["id"], b),
                                        cell_entry(results, config["id"], b)) +
                             jit_mark(config, b))
        rows.append(cells)
    out += ["", f"**Speedup over `{base['id']}`** (median time of one `run()` call, per-run "
            "totals, higher is faster):", "", table(["Configuration"] + benchmarks, rows)]

    # Table 2: marginal speedup over the previous row.
    rows = []
    for i, config in enumerate(configs):
        cells = [row_name(config)]
        for b in benchmarks:
            if config["id"] not in results:
                cells.append("not measured")
            elif i == 0:
                cells.append("—")
            else:
                cells.append(ratio_cell(cell_entry(results, configs[i - 1]["id"], b),
                                        cell_entry(results, config["id"], b)) +
                             jit_mark(config, b))
        rows.append(cells)
    out += ["", "**Marginal speedup over the previous row:**", "",
            table(["Configuration"] + benchmarks, rows)]

    # Table 3: per-iteration p99.
    rows = []
    for config in configs:
        cells = [row_name(config)]
        for b in benchmarks:
            entry = cell_entry(results, config["id"], b)
            if config["id"] not in results:
                cells.append("not measured")
            elif entry is None:
                cells.append("n/a")
            else:
                cells.append(f"{entry['p99_ns'] / 1e6:.2f} ms" +
                             (" " + NOISY_MARK if entry["noisy"] else "") + jit_mark(config, b))
        rows.append(cells)
    out += ["", "**p99 time of one `run()` call** (nearest-rank, over every measured call of "
            "every run pooled; lower is better):", "",
            table(["Configuration"] + benchmarks, rows)]

    # Table 3b: what the JIT cost, next to what it saved (spec §5.6: a JIT that compiles for
    # longer than the program runs is a regression on short programs). Only rows whose results
    # report a compile time (bench.py keeps it only when rung reports one) appear.
    rows = []
    for config in configs:
        entries = [cell_entry(results, config["id"], b) for b in benchmarks]
        if not any(e is not None and "jit_compile_ns_median" in e for e in entries):
            continue
        cells = [row_name(config)]
        for b, entry in zip(benchmarks, entries):
            if entry is None or "jit_compile_ns_median" not in entry:
                cells.append("n/a")
                continue
            compile_ns = entry["jit_compile_ns_median"]
            share = 100.0 * compile_ns / entry["median_total_ns"]
            cells.append(f"{compile_ns / 1e3:.1f} µs ({significant(share)}%)" +
                         jit_mark(config, b))
        rows.append(cells)
    if rows:
        out += ["", "**JIT compile time per process** (median over the runs of the total time "
                "the JIT spent deciding on and compiling functions in one process, rejected ones "
                "included; in parentheses, its share of the median total time of the timed "
                "calls):", "", table(["Configuration"] + benchmarks, rows)]

    # Table 3c: background compilation against compiling inline (notes D8), from the
    # configurations that are not ladder rows.
    if extras:
        out += ["", render_background(results, extras, list(load_expected(root)), not_jit)]

    # Table 4: where each row came from.
    rows = []
    for config in configs + extras:
        data = results.get(config["id"])
        if data is None:
            continue
        meta = data["meta"]
        rows.append([row_name(config),
                     "`" + meta["commit"][:10] + "`" + (" (dirty)" if meta.get("dirty") else ""),
                     meta["date"][:10], machine_text(meta),
                     meta["power_source"].replace("|", "\\|"),
                     f"{meta['kept_runs']} runs of {meta['iterations']} calls"])
    out += ["", "**Provenance** (from `meta` in each results file):", "",
            table(["Configuration", "Commit", "Date", "Machine", "Power", "Measured"], rows)]

    out += ["", f"{REGRESSION_MARK} marks a configuration slower than the one it is compared "
            f"with. {NOISY_MARK} marks a cell where the spread (IQR) of the per-run totals "
            "exceeded 5% of their median in at least one of the two runs compared, so the "
            "figure is uncertain and the row should be re-measured."]
    marked = [b for b in benchmarks if b in not_jit]
    if marked and any(is_jit_config(c) and c["id"] in results for c in configs + extras):
        reasons = "; ".join(f"`{b}`: {not_jit[b]}" for b in marked)
        out += ["", f"{NOT_JIT_MARK} marks a JIT row's cell for a benchmark in which the JIT "
                "compiles no function at its default threshold, so the row measures the "
                "register VM plus the JIT's hotness counting, not machine code (notes D7, D16; "
                "`bench/jit_not_compiled.json`, checked against `rung --jit-log` by a ctest). "
                f"Why: {reasons}."]
    return "\n".join(out)


def ms(ns):
    return f"{ns / 1e6:.2f}"


def render_background(results, extras, benchmarks, not_jit):
    """The background-compilation table: p50, p99 and max of one run() call for each extra
    configuration (synchronous JIT and --jit-background, notes D8), one row per benchmark.
    Times only, no verdict: whether the tail moved is for docs/notes.md section 5 to say."""
    named = " against ".join(f"`{c['id']}` ({c['label']})".replace("|", "\\|") for c in extras)
    measured = [c for c in extras if c["id"] in results]
    title = ("**Background compilation** (notes D8): " + named + ". These are not ladder rows. "
             "Time of one `run()` call, p50 / p99 / max in ms, nearest-rank over every measured "
             "call of every run pooled; lower is better.")
    if not measured:
        return (title + " Not measured yet: `python3 scripts/bench.py --configs " +
                " ".join(c["id"] for c in extras) + "` measures them together (notes D5).")
    dates = {results[c["id"]]["meta"]["date"] for c in measured}
    if len(measured) < len(extras):
        title += " Only " + ", ".join(f"`{c['id']}`" for c in measured) + " has been measured."
    elif len(dates) == 1:
        title += (" Measured in one `bench.py` invocation, so interleaved (notes D5) and "
                  "comparable call for call.")
    else:
        title += (" **Measured in different `bench.py` invocations, so not interleaved:** "
                  "compare with care.")
    header = ["Benchmark"] + [f"`{c['id']}` {stat}" for c in extras
                              for stat in ("p50", "p99", "max")]
    rows = []
    for b in benchmarks:
        entries = [cell_entry(results, c["id"], b) for c in extras]
        if all(e is None for e in entries):
            continue
        mark = " " + NOT_JIT_MARK if b in not_jit else ""
        cells = [f"`{b}`{mark}"]
        for config, entry in zip(extras, entries):
            if entry is None:
                cells += ["not measured"] * 3
                continue
            noisy = " " + NOISY_MARK if entry["noisy"] else ""
            cells += [ms(entry["p50_ns"]) + noisy, ms(entry["p99_ns"]) + noisy,
                      ms(entry["max_ns"]) + noisy]
        rows.append(cells)
    return title + "\n\n" + table(header, rows)


def speedup_range(results, base_id, config_id, benchmarks):
    """'1.30× (strcat) to 4.99× (sieve)': the smallest and largest speedup of one row over
    another, each with its benchmark, or None if no benchmark was measured in both."""
    ratios = []
    for b in benchmarks:
        base, entry = cell_entry(results, base_id, b), cell_entry(results, config_id, b)
        if base is not None and entry is not None:
            ratio = per_iteration_ns(base) / per_iteration_ns(entry)
            ratios.append((ratio, b, base["noisy"] or entry["noisy"]))
    if not ratios:
        return None
    ratios.sort()

    def text(item):
        ratio, b, noisy = item
        return f"{ratio:.2f}{TIMES} (`{b}`{', ' + NOISY_MARK if noisy else ''})"

    if len(ratios) == 1:
        return text(ratios[0])
    return f"{text(ratios[0])} to {text(ratios[-1])}"


def render_summary(root):
    """The generated one-paragraph summary for the top of the README (without the markers).
    Only the numbers are generated; what they mean is explained in docs/notes.md section 5."""
    configs = [c for c in load_configs(root) if is_ladder_row(c)]
    benchmarks = list(load_expected(root))
    results = load_results(root, configs)
    base = configs[0]
    measured = [c for c in configs[1:] if c["id"] in results]
    if base["id"] not in results or not measured:
        return ("_Generated by `scripts/ladder.py`._ No speedups have been measured yet: the "
                "summary appears here once `scripts/bench.py` has measured the baseline and at "
                "least one other row.")
    sentences = []
    shown = [measured[0]] + ([measured[-1]] if measured[-1] is not measured[0] else [])
    for config in shown:
        span = speedup_range(results, base["id"], config["id"], benchmarks)
        if span is not None:
            sentences.append(f"{row_name(config)}: {span} the speed of {row_name(base)}")
    # The largest single-rung loss, so the summary cannot read as "every rung helped".
    worst = None
    for i, config in enumerate(configs[1:], start=1):
        prev = configs[i - 1]
        for b in benchmarks:
            a, e = cell_entry(results, prev["id"], b), cell_entry(results, config["id"], b)
            if a is None or e is None:
                continue
            ratio = per_iteration_ns(a) / per_iteration_ns(e)
            if ratio < 1.0 and (worst is None or ratio < worst[0]):
                worst = (ratio, config, b, a["noisy"] or e["noisy"])
    text = ("_Generated by `scripts/ladder.py` from `results/`, measured on "
            f"{machine_text(results[base['id']]['meta'])}; median time of one `run()` call._ "
            + " ".join(s + "." for s in sentences))
    if worst is not None:
        ratio, config, b, noisy = worst
        text += (f" The largest loss from a single rung is `{config['id']}` on `{b}`: "
                 f"{ratio:.2f}{TIMES} the speed of the row before it"
                 f"{' (' + NOISY_MARK + ', uncertain)' if noisy else ''}.")
    return text


# ----------------------------------------------------------------------------- README


def split_readme(text, start_marker=START_MARKER, end_marker=END_MARKER):
    """Returns (before, current block, after) around the markers."""
    start, end = text.find(start_marker), text.find(end_marker)
    if start < 0 or end < 0 or end < start or text.count(start_marker) != 1 \
            or text.count(end_marker) != 1:
        raise LadderError(f"README.md needs exactly one {start_marker} followed by one "
                          f"{end_marker}")
    before = text[:start + len(start_marker)]
    block = text[start + len(start_marker):end]
    return before, block, text[end:]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero if README.md does not match the results files")
    parser.add_argument("--root", default=SCRIPT_ROOT, help=argparse.SUPPRESS)  # for tests
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)
    readme_path = os.path.join(args.root, "README.md")
    blocks = [(START_MARKER, END_MARKER, render),
              (SUMMARY_START_MARKER, SUMMARY_END_MARKER, render_summary)]
    try:
        with open(readme_path, encoding="utf-8") as f:
            readme = f.read()
        updated = readme
        stale = []  # (current, wanted) of every block that differs
        for start_marker, end_marker, generate in blocks:
            before, current, after = split_readme(updated, start_marker, end_marker)
            wanted = "\n" + generate(args.root) + "\n"
            if current != wanted:
                stale.append((current, wanted))
            updated = before + wanted + after
    except (LadderError, BenchError, OSError) as e:
        print("ladder.py: " + str(e), file=sys.stderr)
        return 2

    if args.check:
        if not stale:
            print("README.md results table and summary match results/*.json")
            return 0
        for current, wanted in stale:
            diff = difflib.unified_diff(current.splitlines(), wanted.splitlines(),
                                        "README.md (committed)", "generated from results/",
                                        lineterm="", n=1)
            print("\n".join(diff), file=sys.stderr)
        print("\nladder.py: the README results table or summary is not what results/*.json "
              "generate. Never edit it by hand; run `python3 scripts/ladder.py` and commit the "
              "result.", file=sys.stderr)
        return 1

    if not stale:
        print("README.md is already up to date")
        return 0
    with open(readme_path, "w", encoding="utf-8") as f:
        f.write(updated)
    print("updated the results table and summary in README.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
