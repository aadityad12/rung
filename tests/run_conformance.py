#!/usr/bin/env python3
"""Conformance runner: checks one Rung engine against tests/conformance/**/*.rg.

Usage:
    run_conformance.py --rung PATH --engine NAME [--gc-stress] [--timeout SEC] [--jobs N]
                       [FILTER...]

The expected behaviour of each test lives in comments inside the test file (docs/notes.md D13):

    // expect: TEXT                     one line of stdout ("// expect:" alone is an empty line)
    // expect runtime error: MSG        stderr line 1 is "[line L] runtime error: MSG", exit 70
    // expect compile error: MSG        stderr line 1 is "[line L] compile error: MSG", exit 65

L is the line the comment is on. With no error annotation the test must exit 0 with empty stderr.
Stdout, exit code and the first line of stderr are compared exactly.

FILTER arguments are substrings of a test's path relative to tests/conformance (for example
"numbers/" or "int_division"); a test runs if it matches any of them.

Exit status: 0 if every test passed, 1 if any failed (or nothing matched), 2 on bad usage.
Python 3 standard library only.
"""

import argparse
import concurrent.futures
import difflib
import os
import re
import subprocess
import sys

SUITE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "conformance")

EXIT_RUNTIME_ERROR = 70  # sysexits.h, docs/notes.md D9
EXIT_COMPILE_ERROR = 65

# One space after the colon belongs to the syntax; anything after it is the text, verbatim.
EXPECT_RE = re.compile(r"//\s*expect:(?: (.*))?$")
ERROR_RE = re.compile(r"//\s*expect (runtime|compile) error: ?(.*)$")


class Expectation:
    def __init__(self):
        self.stdout_lines = []
        self.error_kind = None  # None, "runtime" or "compile"
        self.error_line = None
        self.error_message = None
        self.problem = None  # set when the annotations themselves are malformed

    @property
    def exit_code(self):
        if self.error_kind == "runtime":
            return EXIT_RUNTIME_ERROR
        if self.error_kind == "compile":
            return EXIT_COMPILE_ERROR
        return 0

    @property
    def stdout(self):
        return "".join(line + "\n" for line in self.stdout_lines)

    @property
    def stderr_first_line(self):
        if self.error_kind is None:
            return ""
        return "[line %d] %s error: %s" % (self.error_line, self.error_kind, self.error_message)


def parse_expectation(path):
    """Reads the annotations of one test file. The comment can follow code on its line."""
    expect = Expectation()
    with open(path, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    for number, line in enumerate(text.split("\n"), start=1):
        line = line.rstrip("\r")
        # Error annotations first: "expect runtime error:" also contains "expect".
        error = ERROR_RE.search(line)
        if error:
            if expect.error_kind is not None:
                expect.problem = "more than one error annotation (line %d)" % number
            expect.error_kind = error.group(1)
            expect.error_line = number
            expect.error_message = error.group(2)
            continue
        plain = EXPECT_RE.search(line)
        if plain:
            expect.stdout_lines.append(plain.group(1) or "")
    return expect


def find_tests(filters):
    tests = []
    for root, _dirs, files in os.walk(SUITE_DIR):
        for name in files:
            if name.endswith(".rg"):
                path = os.path.join(root, name)
                tests.append((os.path.relpath(path, SUITE_DIR).replace(os.sep, "/"), path))
    tests.sort()
    if filters:
        tests = [t for t in tests if any(f in t[0] for f in filters)]
    return tests


def diff_text(expected, actual, label):
    lines = list(difflib.unified_diff(
        expected.splitlines(), actual.splitlines(),
        fromfile="expected " + label, tofile="actual " + label, lineterm="", n=1))
    if not lines:
        # The texts differ only in something splitlines() hides (a missing final newline).
        return "    expected %s: %r\n    actual   %s: %r" % (label, expected, label, actual)
    return "\n".join("    " + line for line in lines)


def run_one(args, name, path):
    """Returns (name, None) on success, or (name, failure_text)."""
    expect = parse_expectation(path)
    if expect.problem:
        return name, "  bad test file: " + expect.problem

    command = [args.rung, "--engine=" + args.engine]
    if args.gc_stress:
        command.append("--gc-stress")
    command.append(path)
    try:
        done = subprocess.run(command, capture_output=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        return name, "  timed out after %g s (infinite loop, or --timeout too small)" % args.timeout

    stdout = done.stdout.decode("utf-8", errors="replace")
    stderr = done.stderr.decode("utf-8", errors="replace")
    stderr_first = stderr.split("\n", 1)[0]

    problems = []
    if done.returncode != expect.exit_code:
        problems.append("  exit code: expected %d, got %d" % (expect.exit_code, done.returncode))
    if stdout != expect.stdout:
        problems.append("  stdout differs:\n" + diff_text(expect.stdout, stdout, "stdout"))
    if stderr_first != expect.stderr_first_line:
        problems.append("  stderr first line: expected %r, got %r"
                        % (expect.stderr_first_line, stderr_first))
    if problems:
        return name, "\n".join(problems)
    return name, None


def main():
    parser = argparse.ArgumentParser(description="Run the Rung conformance suite on one engine.")
    parser.add_argument("--rung", required=True, help="path to the rung executable")
    parser.add_argument("--engine", required=True, help="engine name passed as --engine=NAME")
    parser.add_argument("--gc-stress", action="store_true", help="pass --gc-stress to rung")
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="seconds allowed per test (default 10)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                        help="tests to run in parallel (default: CPU count)")
    parser.add_argument("filters", nargs="*", metavar="FILTER",
                        help="only run tests whose path contains one of these")
    args = parser.parse_args()

    if not os.path.isfile(args.rung):
        print("run_conformance: no such executable: %s" % args.rung, file=sys.stderr)
        return 2

    tests = find_tests(args.filters)
    if not tests:
        print("run_conformance: no tests matched", file=sys.stderr)
        return 1

    # Results are printed in path order however the tests finish, so runs are comparable.
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        results = list(pool.map(lambda t: run_one(args, t[0], t[1]), tests))

    failed = 0
    for name, failure in results:
        if failure is not None:
            failed += 1
            print("FAIL %s\n%s" % (name, failure))
    print("%d passed, %d failed" % (len(results) - failed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
