#!/usr/bin/env python3
"""Tests for scripts/bench.py and scripts/ladder.py (issue #8).

None of these measure anything real: bench.py is driven against a fake `rung` that reports
made-up, fixed iteration times, so the pipeline's logic (statistics, interleaving, the dirty-tree
refusal, result checking, the README table and --check) is tested without a quiet machine.

    python3 tests/test_bench_scripts.py
"""
import contextlib
import io
import json
import os
import random
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

import bench  # noqa: E402
import ladder  # noqa: E402

FAKE_RUNG = f"""#!{sys.executable}
import json, os, sys
opts = dict(a.split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
source = sys.argv[-1]
expected = json.load(open(os.path.join(os.path.dirname(source), "expected.json")))
name = os.path.basename(source)[:-3]
result = expected[name] if opts.get("--fake-result") is None else opts["--fake-result"]
ns = int(opts.get("--fake-ns", "1000"))
json.dump({{"engine": opts.get("--engine", "?"), "iterations_ns": [ns] * int(opts["--bench"]),
          "result": result, "heap": {{"collections": 0}}}}, open(opts["--bench-out"], "w"))
"""


def run_main(module, argv):
    """Runs module.main(argv) and returns (exit code, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = module.main(argv)
    return code, out.getvalue(), err.getvalue()


def git(root, *args):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], cwd=root,
                   check=True, capture_output=True)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


class StatisticsTest(unittest.TestCase):
    def test_median_and_iqr(self):
        self.assertEqual(bench.median([5, 1, 3]), 3)
        self.assertEqual(bench.iqr([1, 2, 3, 4, 5]), 2)  # Q1 = 2, Q3 = 4
        self.assertEqual(bench.iqr([7]), 0)

    def test_percentile_is_nearest_rank(self):
        values = list(range(1, 201))  # 200 samples
        self.assertEqual(bench.percentile(values, 50), 100)
        self.assertEqual(bench.percentile(values, 99), 198)
        self.assertEqual(bench.percentile([42], 99), 42)
        self.assertEqual(bench.percentile([3, 1, 2], 99), 3)

    def make_runs(self, totals):
        return [{"iterations_ns": [t // 2, t - t // 2], "wall_ns": t + 5, "peak_rss_bytes": t}
                for t in totals]

    def test_summary_values(self):
        s = bench.summarize(self.make_runs([100, 102, 98, 101, 99]), "7", 2)
        self.assertEqual(s["median_total_ns"], 100)
        self.assertEqual(s["iqr_total_ns"], 2)
        self.assertFalse(s["noisy"])
        self.assertEqual(s["max_ns"], 51)
        self.assertEqual(s["peak_rss_bytes_max"], 102)
        self.assertEqual(s["wall_median_ns"], 105)
        self.assertEqual(len(s["runs"]), 5)

    def test_jit_compile_time_is_kept_only_when_reported(self):
        self.assertNotIn("jit_compile_ns_median", bench.summarize(self.make_runs([100]), "7", 2))
        runs = self.make_runs([100, 102, 98])
        for run, ns in zip(runs, [300, 100, 200]):
            run["jit_compile_ns"] = ns
        s = bench.summarize(runs, "7", 2)
        self.assertEqual(s["jit_compile_ns_median"], 200)
        self.assertEqual([r["jit_compile_ns"] for r in s["runs"]], [300, 100, 200])

    def test_noisy_when_iqr_exceeds_five_percent_of_median(self):
        # IQR 10 on median 100 is 10%: noisy. IQR 5 is exactly 5%: not noisy (the limit is >).
        self.assertTrue(bench.summarize(self.make_runs([90, 95, 100, 105, 110]), "7", 2)["noisy"])
        self.assertFalse(bench.summarize(self.make_runs([95, 97, 100, 102, 105]), "7", 2)["noisy"])


class InterleavingTest(unittest.TestCase):
    def test_same_seed_same_order_and_every_round_covers_every_pair(self):
        orders = []
        for _ in range(2):
            seen = []
            real = bench.run_process

            def fake(root, preset, args, benchmark, iterations, seen=seen):
                seen.append((args[0], benchmark))
                return {"iterations_ns": [1] * iterations, "result": "ok", "wall_ns": 1,
                        "peak_rss_bytes": 1, "heap": None}
            bench.run_process = fake
            try:
                configs = [{"id": c, "preset": "p", "args": [c]} for c in "ABC"]
                bench.measure("/", configs, ["x", "y"], {"x": "ok", "y": "ok"}, 4, 3, 99,
                              log=lambda *a, **k: None)
            finally:
                bench.run_process = real
            orders.append(seen)
        self.assertEqual(orders[0], orders[1])
        pairs = sorted((c, b) for c in "ABC" for b in "xy")
        for r in range(4):
            self.assertEqual(sorted(orders[0][r * 6:(r + 1) * 6]), pairs)
        # Shuffled, not grouped A A A B B B: some round differs from the plain order.
        self.assertTrue(any(orders[0][r * 6:(r + 1) * 6] != pairs for r in range(4)))

    def test_seed_changes_order(self):
        a, b = list(range(20)), list(range(20))
        random.Random(1).shuffle(a)
        random.Random(2).shuffle(b)
        self.assertNotEqual(a, b)


class BenchEndToEndTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        self.addCleanup(self._tmp.cleanup)
        write(os.path.join(self.root, "scripts", "ladder_configs.json"), json.dumps([
            {"id": "01_fast", "label": "Fast", "preset": "release",
             "args": ["--engine=fake", "--fake-ns=1000"]},
            {"id": "02_slow", "label": "Slow", "preset": "release",
             "args": ["--engine=fake", "--fake-ns=4000"]}]))
        write(os.path.join(self.root, "bench", "expected.json"),
              json.dumps({"alpha": "11", "beta": "22"}))
        write(os.path.join(self.root, "bench", "alpha.rg"), "fn run() { return 11; }\n")
        write(os.path.join(self.root, "bench", "beta.rg"), "fn run() { return 22; }\n")
        write(os.path.join(self.root, ".gitignore"), "build/\n")
        rung = os.path.join(self.root, "build", "release", "rung")
        write(rung, FAKE_RUNG)
        os.chmod(rung, os.stat(rung).st_mode | stat.S_IXUSR)
        git(self.root, "init", "-q")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "fixture")

    def run_bench(self, *extra):
        return run_main(bench, ["--root", self.root, "--no-build", "--runs", "3",
                                "--iterations", "4", *extra])

    def results(self, config_id):
        with open(os.path.join(self.root, "results", config_id + ".json")) as f:
            return json.load(f)

    def test_writes_results_with_metadata_and_statistics(self):
        code, out, err = self.run_bench()
        self.assertEqual(code, 0, err)
        fast = self.results("01_fast")
        meta = fast["meta"]
        for key in ("cpu", "os", "power_source", "compiler", "commit", "date"):
            self.assertTrue(meta[key], key)
        self.assertEqual((meta["runs"], meta["kept_runs"], meta["iterations"], meta["seed"],
                          meta["dirty"]), (3, 2, 4, bench.DEFAULT_SEED, False))
        alpha = fast["benchmarks"]["alpha"]
        self.assertEqual(alpha["median_total_ns"], 4000)
        self.assertEqual(alpha["p99_ns"], 1000)
        self.assertEqual(len(alpha["runs"]), 2)  # warm-up round discarded
        self.assertGreater(alpha["peak_rss_bytes_max"], 0)
        self.assertEqual(self.results("02_slow")["benchmarks"]["beta"]["median_total_ns"], 16000)

    def test_subset_selection(self):
        code, _, err = self.run_bench("--configs", "02_slow", "--benchmarks", "beta")
        self.assertEqual(code, 0, err)
        self.assertFalse(os.path.exists(os.path.join(self.root, "results", "01_fast.json")))
        self.assertEqual(list(self.results("02_slow")["benchmarks"]), ["beta"])

    def test_unknown_config_is_an_error(self):
        code, _, err = self.run_bench("--configs", "nope")
        self.assertEqual(code, 1)
        self.assertIn("unknown configuration 'nope'", err)

    def test_refuses_dirty_tree_unless_allowed(self):
        write(os.path.join(self.root, "bench", "alpha.rg"), "fn run() { return 12; }\n")
        code, _, err = self.run_bench()
        self.assertEqual(code, 1)
        self.assertIn("uncommitted changes", err)
        self.assertFalse(os.path.exists(os.path.join(self.root, "results")))
        code, _, err = self.run_bench("--allow-dirty")
        self.assertEqual(code, 0, err)
        self.assertTrue(self.results("01_fast")["meta"]["dirty"])

    def test_results_directory_does_not_count_as_dirty(self):
        self.assertEqual(self.run_bench()[0], 0)
        code, _, err = self.run_bench()  # results/ now exists, untracked
        self.assertEqual(code, 0, err)

    def test_wrong_result_aborts(self):
        write(os.path.join(self.root, "scripts", "ladder_configs.json"), json.dumps([
            {"id": "01_bad", "label": "Bad", "preset": "release",
             "args": ["--engine=fake", "--fake-result=999"]}]))
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "bad config")
        code, _, err = self.run_bench()
        self.assertEqual(code, 1)
        self.assertIn("result '999', expected", err)
        self.assertFalse(os.path.exists(os.path.join(self.root, "results", "01_bad.json")))

    def test_replacing_a_results_file_prints_what_changed(self):
        self.run_bench()
        path = os.path.join(self.root, "results", "01_fast.json")
        old = self.results("01_fast")
        old["benchmarks"]["alpha"]["median_total_ns"] = 8000
        old["benchmarks"]["gone"] = old["benchmarks"]["beta"]
        write(path, json.dumps(old))
        code, out, _ = self.run_bench()
        self.assertEqual(code, 0)
        self.assertIn("replacing results/01_fast.json", out)
        self.assertIn("alpha: 0.002 ms -> 0.001 ms per iteration (-50.0%)", out)
        self.assertIn("gone: DROPPED", out)

    def test_too_few_runs_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            bench.parse_args(["--runs", "1"])


def synthetic_results(root, config, per_iter_ns, noisy=False, p99=None, benchmarks=("alpha",
                                                                                    "beta")):
    """Writes results/<id>.json as bench.py would, from made-up per-iteration times."""
    entries = {}
    for name in benchmarks:
        total = per_iter_ns[name] * 10
        runs = [{"iterations_ns": [per_iter_ns[name]] * 10, "wall_ns": total,
                 "peak_rss_bytes": 1} for _ in range(3)]
        entry = bench.summarize(runs, "x", 10)
        entry["noisy"] = noisy
        if p99:
            entry["p99_ns"] = p99
        entries[name] = entry
    write(os.path.join(root, "results", config["id"] + ".json"), json.dumps({
        "config": config,
        "meta": {"cpu": "Fake CPU", "os": "FakeOS 1", "power_source": "AC", "compiler": "cc",
                 "commit": "0123456789abcdef", "dirty": False, "date": "2026-10-01T10:00:00Z",
                 "runs": 4, "kept_runs": 3, "iterations": 10, "seed": 1},
        "benchmarks": entries}))


class LadderTest(unittest.TestCase):
    CONFIGS = [{"id": "01_a", "label": "Base", "preset": "release", "args": []},
               {"id": "02_b", "label": "Faster", "preset": "release", "args": []},
               {"id": "03_c", "label": "Slower", "preset": "release", "args": []},
               {"id": "04_d", "label": "Unmeasured", "preset": "release", "args": []}]

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        self.addCleanup(self._tmp.cleanup)
        write(os.path.join(self.root, "scripts", "ladder_configs.json"), json.dumps(self.CONFIGS))
        write(os.path.join(self.root, "bench", "expected.json"),
              json.dumps({"beta": "1", "alpha": "2"}))
        write(os.path.join(self.root, "README.md"),
              f"# T\n\n{ladder.SUMMARY_START_MARKER}\nold\n{ladder.SUMMARY_END_MARKER}\n\n"
              f"## Results\n\n{ladder.START_MARKER}\nstale\n{ladder.END_MARKER}\n\n"
              "## After\n")

    def readme(self):
        with open(os.path.join(self.root, "README.md"), encoding="utf-8") as f:
            return f.read()

    def fill(self):
        synthetic_results(self.root, self.CONFIGS[0], {"alpha": 1000, "beta": 2000})
        synthetic_results(self.root, self.CONFIGS[1], {"alpha": 500, "beta": 400}, p99=777000)
        synthetic_results(self.root, self.CONFIGS[2], {"alpha": 1000, "beta": 400}, noisy=True)

    def test_no_results_says_so(self):
        code, _, _ = run_main(ladder, ["--root", self.root])
        self.assertEqual(code, 0)
        self.assertIn("No results have been recorded yet", self.readme())
        self.assertEqual(run_main(ladder, ["--check", "--root", self.root])[0], 0)

    def test_tables(self):
        self.fill()
        self.assertEqual(run_main(ladder, ["--root", self.root])[0], 0)
        text = self.readme()
        # Columns follow bench/expected.json order, not alphabetical.
        self.assertIn("| Configuration | beta | alpha |", text)
        self.assertIn("| `01_a` Base | 1.00× | 1.00× |", text)
        self.assertIn("| `02_b` Faster | 5.00× | 2.00× |", text)
        # Marginal: 03_c is half as fast as 02_b on alpha (regression), same on beta.
        self.assertIn("| `03_c` Slower | 1.00× † | **0.50× ▼** † |", text)
        self.assertIn("| `01_a` Base | — | — |", text)
        self.assertIn("| `04_d` Unmeasured | not measured | not measured |", text)
        self.assertIn("0.78 ms", text)  # p99 table
        self.assertIn("Fake CPU, FakeOS 1", text)
        self.assertIn("`0123456789`", text)
        self.assertTrue(text.endswith("## After\n"))
        self.assertIn("## Results\n\n" + ladder.START_MARKER + "\n_This section is generated",
                      text)

    def test_summary(self):
        self.fill()
        self.assertEqual(run_main(ladder, ["--root", self.root])[0], 0)
        text = self.readme()
        start = text.index(ladder.SUMMARY_START_MARKER)
        summary = text[start:text.index(ladder.SUMMARY_END_MARKER)]
        # The first row above the baseline and the last measured row, each as a range with the
        # benchmark at each end; noisy cells say so.
        self.assertIn("`02_b` Faster: 2.00× (`alpha`) to 5.00× (`beta`) the speed of "
                      "`01_a` Base.", summary)
        self.assertIn("`03_c` Slower: 1.00× (`alpha`, †) to 5.00× (`beta`, †)", summary)
        # The worst single-rung loss is always named.
        self.assertIn("The largest loss from a single rung is `03_c` on `alpha`: 0.50×", summary)
        self.assertIn("Fake CPU, FakeOS 1", summary)
        self.assertNotIn("old", summary)

    def test_summary_without_results_says_so(self):
        self.assertEqual(run_main(ladder, ["--root", self.root])[0], 0)
        self.assertIn("No speedups have been measured yet", self.readme())

    def test_summary_hand_edit_fails_check(self):
        self.fill()
        run_main(ladder, ["--root", self.root])
        write(os.path.join(self.root, "README.md"), self.readme().replace("5.00× (`beta`) the",
                                                                          "6.00× (`beta`) the"))
        code, _, err = run_main(ladder, ["--check", "--root", self.root])
        self.assertEqual(code, 1)
        self.assertIn("6.00", err)

    def test_jit_rows_show_compile_time_and_mark_benchmarks_not_compiled(self):
        configs = self.CONFIGS[:2] + [
            {"id": "03_c", "label": "JIT", "preset": "release", "args": ["--engine=jit"]}]
        write(os.path.join(self.root, "scripts", "ladder_configs.json"), json.dumps(configs))
        write(os.path.join(self.root, "bench", "jit_not_compiled.json"),
              json.dumps({"beta": "rejected for a reason"}))
        self.fill()
        path = os.path.join(self.root, "results", "03_c.json")
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        data["config"] = configs[2]
        data["benchmarks"]["alpha"]["jit_compile_ns_median"] = 2500.0   # of 10,000 ns in total
        data["benchmarks"]["beta"]["jit_compile_ns_median"] = 0.38      # of 4,000 ns
        write(path, json.dumps(data))
        self.assertEqual(run_main(ladder, ["--root", self.root])[0], 0)
        text = self.readme()
        self.assertIn("**JIT compile time per process**", text)
        # Compile time next to its share of the timed calls; tiny shares keep two significant
        # figures instead of rounding to zero.
        self.assertIn("| `03_c` JIT | 0.0 µs (0.0095%) ‡ | 2.5 µs (25%) |", text)
        # Only the JIT row is marked, in every table.
        self.assertIn("| `03_c` JIT | 5.00× † ‡ | 1.00× † |", text)
        self.assertIn("| `02_b` Faster | 5.00× | 2.00× |", text)
        self.assertIn("‡ marks a JIT row's cell", text)
        self.assertIn("`beta`: rejected for a reason", text)

    def test_background_comparison_is_its_own_table(self):
        # Configurations marked "ladder": false (notes D8's sync / background pair) get a table
        # of p50 / p99 / max per benchmark, and stay out of the speedup tables and the summary.
        extras = [{"id": "x_sync", "label": "Sync", "ladder": False, "preset": "release",
                   "args": ["--engine=jit"]},
                  {"id": "x_bg", "label": "Background", "ladder": False, "preset": "release",
                   "args": ["--engine=jit", "--jit-background"]}]
        write(os.path.join(self.root, "scripts", "ladder_configs.json"),
              json.dumps(self.CONFIGS + extras))
        write(os.path.join(self.root, "bench", "expected.json"),
              json.dumps({"beta": "1", "alpha": "2", "warm": "3"}))
        self.fill()
        run_main(ladder, ["--root", self.root])
        self.assertIn("Not measured yet: `python3 scripts/bench.py --configs x_sync x_bg`",
                      self.readme())

        names = ("alpha", "beta", "warm")
        synthetic_results(self.root, extras[0], {"alpha": 1000, "beta": 2000, "warm": 3000},
                          p99=5_000_000, benchmarks=names)
        synthetic_results(self.root, extras[1], {"alpha": 1000, "beta": 2000, "warm": 3000},
                          p99=4_000_000, benchmarks=names)
        self.assertEqual(run_main(ladder, ["--root", self.root])[0], 0)
        text = self.readme()
        self.assertIn("**Background compilation** (notes D8): `x_sync` (Sync) against `x_bg` "
                      "(Background).", text)
        self.assertIn("Measured in one `bench.py` invocation", text)
        self.assertIn("| Benchmark | `x_sync` p50 | `x_sync` p99 | `x_sync` max | `x_bg` p50 | "
                      "`x_bg` p99 | `x_bg` max |", text)
        self.assertIn("| `warm` | 0.00 | 5.00 | 0.00 | 0.00 | 4.00 | 0.00 |", text)
        # Only the extras measured `warm`, so the ladder tables get no column for it.
        self.assertIn("| Configuration | beta | alpha |", text)
        self.assertNotIn("x_sync` Sync |", text.split("**Background compilation**")[0])
        # Both are listed under provenance.
        self.assertIn("| `x_bg` Background | `0123456789` |", text)
        start = text.index(ladder.SUMMARY_START_MARKER)
        self.assertNotIn("x_", text[start:text.index(ladder.SUMMARY_END_MARKER)])

        # Results from two invocations are not interleaved, and the table says so.
        path = os.path.join(self.root, "results", "x_bg.json")
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        data["meta"]["date"] = "2026-10-02T10:00:00Z"
        write(path, json.dumps(data))
        run_main(ladder, ["--root", self.root])
        self.assertIn("Measured in different `bench.py` invocations, so not interleaved",
                      self.readme())

    def test_no_compile_time_table_without_jit_results(self):
        self.fill()
        self.assertEqual(run_main(ladder, ["--root", self.root])[0], 0)
        self.assertNotIn("JIT compile time", self.readme())
        self.assertNotIn("‡", self.readme())

    def test_check_passes_when_current_and_fails_after_hand_edit(self):
        self.fill()
        run_main(ladder, ["--root", self.root])
        code, out, _ = run_main(ladder, ["--check", "--root", self.root])
        self.assertEqual(code, 0, out)
        edited = self.readme().replace("5.00×", "9.99×")
        write(os.path.join(self.root, "README.md"), edited)
        code, _, err = run_main(ladder, ["--check", "--root", self.root])
        self.assertEqual(code, 1)
        self.assertIn("9.99", err)
        self.assertIn("Never edit it by hand", err)
        # --check must not repair the file.
        self.assertEqual(self.readme(), edited)

    def test_check_fails_when_results_change(self):
        self.fill()
        run_main(ladder, ["--root", self.root])
        synthetic_results(self.root, self.CONFIGS[1], {"alpha": 250, "beta": 400})
        self.assertEqual(run_main(ladder, ["--check", "--root", self.root])[0], 1)
        self.assertEqual(run_main(ladder, ["--root", self.root])[0], 0)
        self.assertEqual(run_main(ladder, ["--check", "--root", self.root])[0], 0)

    def test_missing_markers_is_an_error(self):
        write(os.path.join(self.root, "README.md"), "# nothing here\n")
        code, _, err = run_main(ladder, ["--check", "--root", self.root])
        self.assertEqual(code, 2)
        self.assertIn("ladder:start", err)

    def test_corrupt_results_file_is_an_error(self):
        write(os.path.join(self.root, "results", "01_a.json"), "{not json")
        code, _, err = run_main(ladder, ["--root", self.root])
        self.assertEqual(code, 2)
        self.assertIn("01_a.json", err)

    def test_repository_readme_matches_committed_results(self):
        # The same check CI runs, against the real README and results/.
        code, out, err = run_main(ladder, ["--check"])
        self.assertEqual(code, 0, err)


class RepositoryConfigsTest(unittest.TestCase):
    """The real scripts/ladder_configs.json against notes D4 and CMakePresets.json.

    bench.py only discovers a bad row (an unknown preset, a flag the engine refuses) when the
    owner is already on an exclusive machine, so the file is checked here instead."""

    # Notes D4's table, in order: what each row adds to the previous one. The comparison is exact,
    # so a row that is missing, reordered, or loses a flag a lower rung added fails the test.
    LADDER = [
        ("01_tree", "release", "tree", []),
        ("02_stack", "release", "stack", []),
        ("03_goto", "release-goto", "stack", []),
        ("04_nanbox", "release-goto-nanbox", "stack", []),
        ("05_register", "release-goto-nanbox", "register", []),
        ("06_super", "release-goto-nanbox", "register", ["--superinstructions"]),
        ("07_ic", "release-goto-nanbox", "register", ["--superinstructions", "--inline-cache"]),
        ("08_fold", "release-goto-nanbox", "register",
         ["--superinstructions", "--inline-cache", "--fold"]),
        ("09_jit", "release-goto-nanbox", "jit",
         ["--superinstructions", "--inline-cache", "--fold"]),
    ]

    # Flags that only some engines accept (src/main.cpp refuses them elsewhere).
    REGISTER_OR_JIT_ONLY = {"--superinstructions", "--inline-cache"}
    JIT_ONLY = {"--jit-background"}

    def configs(self):
        return bench.load_configs(ROOT)

    def test_rows_are_the_cumulative_ladder(self):
        got = []
        for config in [c for c in self.configs() if ladder.is_ladder_row(c)]:
            engines = [a for a in config["args"] if a.startswith("--engine=")]
            self.assertEqual(len(engines), 1, config["id"])
            flags = [a for a in config["args"] if not a.startswith("--engine=")]
            got.append((config["id"], config["preset"], engines[0][len("--engine="):], flags))
        self.assertEqual(got, self.LADDER)

    def test_every_preset_is_a_real_configure_and_build_preset(self):
        with open(os.path.join(ROOT, "CMakePresets.json"), encoding="utf-8") as f:
            presets = json.load(f)
        configure = {p["name"] for p in presets["configurePresets"] if not p.get("hidden")}
        build = {p["name"] for p in presets["buildPresets"] if not p.get("hidden")}
        for config in self.configs():
            self.assertIn(config["preset"], configure, config["id"])
            self.assertIn(config["preset"], build, config["id"])

    def test_every_flag_is_accepted_by_its_engine(self):
        for config in self.configs():
            engine = [a for a in config["args"] if a.startswith("--engine=")][0].split("=")[1]
            for arg in config["args"]:
                if arg.startswith("--engine="):
                    self.assertIn(engine, ("tree", "stack", "register", "jit"), config["id"])
                elif arg in self.REGISTER_OR_JIT_ONLY:
                    self.assertIn(engine, ("register", "jit"), f"{config['id']}: {arg}")
                elif arg in self.JIT_ONLY:
                    self.assertEqual(engine, "jit", f"{config['id']}: {arg}")
                else:
                    self.assertEqual(arg, "--fold", f"{config['id']}: unknown flag {arg}")

    def test_the_background_pair_differs_from_row_09_only_in_where_it_compiles(self):
        # Notes D8: synchronous against background compilation, measured together. The sync
        # side repeats row 09 exactly, so the pair isolates --jit-background.
        by_id = {c["id"]: c for c in self.configs()}
        sync, background = by_id["jit_sync"], by_id["jit_background"]
        self.assertFalse(ladder.is_ladder_row(sync))
        self.assertFalse(ladder.is_ladder_row(background))
        self.assertEqual(sync["preset"], by_id["09_jit"]["preset"])
        self.assertEqual(sync["args"], by_id["09_jit"]["args"])
        self.assertEqual(background["preset"], sync["preset"])
        self.assertEqual(background["args"], sync["args"] + ["--jit-background"])

    def test_the_jit_row_needs_an_arm64_nanbox_build(self):
        # main.cpp: "--engine=jit" exists only with NaN-boxed values (notes D16).
        jit = [c for c in self.configs() if "--engine=jit" in c["args"]]
        self.assertTrue(all(c["preset"] == "release-goto-nanbox" for c in jit))


if __name__ == "__main__":
    unittest.main()
