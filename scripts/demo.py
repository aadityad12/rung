#!/usr/bin/env python3
"""Run a checked tour of Rung. Python standard library only; works from an installed archive."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rung", default=str(ROOT / "build/debug/rung"),
                        help="executable to demonstrate (default: build/debug/rung)")
    parser.add_argument("--section", choices=("all", "engines", "bytecode", "jit"), default="all")
    parser.add_argument("--capture", type=Path, help="write the verified tour as JSON for rendering")
    parser.add_argument("--check-capture", type=Path,
                        help="fail if the displayed output or examples differ from an earlier capture")
    args = parser.parse_args()
    executable = str(Path(args.rung).resolve())
    records = []

    def run(flags, expected=None):
        done = subprocess.run([executable, *flags], cwd=ROOT, capture_output=True,
                              text=True, timeout=30)
        if done.returncode != 0:
            raise RuntimeError(f"rung {' '.join(flags)} exited {done.returncode}: {done.stderr}")
        if expected is not None and done.stdout != expected:
            raise RuntimeError(f"unexpected output for {flags}: {done.stdout!r}; expected {expected!r}")
        return done

    def record(section, title, caption, flags, done, stdout=None):
        # These clips explain execution, not performance. Keep the real log but omit the
        # variable compile duration; presentation pacing is independent of execution time.
        stderr = re.sub(r" in \d+ ns(?=\n|$)", "", done.stderr)
        item = {"section": section, "title": title, "caption": caption,
                "command": shlex.join(["rung", *flags]),
                "stdout": done.stdout if stdout is None else stdout,
                "stderr": stderr}
        records.append(item)
        print(f"\n{title}\n{caption}\n$ {item['command']}")
        if stderr:
            print("[stderr]\n" + stderr, end="")
        if item["stdout"]:
            print("[stdout]\n" + item["stdout"], end="")

    try:
        version = run(["--version"]).stdout.strip()
        jit = "jit" in version.split("engines:")[-1].split()
        print(version)
        print("\nRung guided demo — each result is checked before it is shown.")

        if args.section in ("all", "engines"):
            modes = [("Tree walker", ["--engine=tree"]),
                     ("Stack VM", ["--engine=stack"]),
                     ("Register VM", ["--engine=register"])]
            if jit:
                modes += [("ARM64 JIT", ["--engine=jit"]),
                          ("Background JIT", ["--engine=jit", "--jit-background"])]
            for title, flags in modes:
                flags = flags + ["examples/hot_loop.rg"]
                done = run(flags, "994657\n994657\n")
                if done.stderr:
                    raise RuntimeError(f"unexpected diagnostics in {title}: {done.stderr}")
                record("engines", title, "One program. Two calls. The same answer.", flags, done)
            print(f"\nPASS: all {len(modes)} available execution configurations agree.")

        if args.section in ("all", "bytecode"):
            for engine in ("stack", "register"):
                run([f"--engine={engine}", "examples/bytecode.rg"], "42\n")
                flags = [f"--engine={engine}", "--dump-bytecode", "examples/bytecode.rg"]
                done = run(flags)
                # The full dump includes the script; focus on the unchanged add function.
                match = re.search(r"^== add .*?(?=\n== |\Z)", done.stdout, re.M | re.S)
                if match is None or "ADD" not in match.group():
                    raise RuntimeError("could not find add's bytecode in the dump")
                record("bytecode", f"{engine.title()} bytecode", "Function excerpt: return a + b;",
                       flags, done, stdout=match.group().rstrip() + "\n")
            print("\nPASS: both bytecode forms execute and return 42.")

        if args.section in ("all", "jit"):
            if not jit:
                print("\nSKIPPED: native JIT demos need ARM64 and a NaN-boxed build.\n"
                      "Use the release-goto-nanbox preset on an ARM64 machine.")
            else:
                flags = ["--engine=jit", "--jit-log", "examples/hot_loop.rg"]
                done = run(flags, "994657\n994657\n")
                if "[jit] compiled sum:" not in done.stderr or "[jit] bail-out" in done.stderr:
                    raise RuntimeError("the hot loop did not compile and run as expected")
                record("jit", "A hot function becomes machine code",
                       "Default threshold: first call heats up; second call enters compiled code.",
                       flags, done)
                flags = ["--engine=jit", "--jit-threshold=1", "--jit-log", "examples/jit_fallback.rg"]
                done = run(flags, "42\n22.5\n")
                if "[jit] compiled add:" not in done.stderr or "[jit] bail-out in add" not in done.stderr:
                    raise RuntimeError("the type-guard fallback was not exercised")
                record("jit", "A float takes the safe path back",
                       "Threshold 1 for this example: add(20, 22), then add(20, 2.5).",
                       flags, done)
                print("\nPASS: native compilation and fallback preserve the answers.")

        examples = {name: hashlib.sha256((ROOT / "examples" / name).read_bytes()).hexdigest()
                    for name in ("hot_loop.rg", "bytecode.rg", "jit_fallback.rg")}
        if args.check_capture:
            previous = json.loads(args.check_capture.read_text(encoding="utf-8"))
            if previous["example_sha256"] != examples:
                raise RuntimeError("demo examples changed; recapture and render the clips")
            expected = {(r["section"], r["title"]): r for r in previous["records"]}
            for item in records:
                if expected.get((item["section"], item["title"])) != item:
                    raise RuntimeError(f"capture is stale for {item['title']}; recapture and render")
            print("\nPASS: captured clips match this build's displayed output.")
        if args.capture:
            capture = {"version": version, "platform": platform.system(),
                       "architecture": platform.machine(), "example_sha256": examples,
                       "presentation": "Real command output; compiler durations omitted. "
                                       "Playback is paced for reading, not execution time.",
                       "records": records}
            args.capture.parent.mkdir(parents=True, exist_ok=True)
            args.capture.write_text(json.dumps(capture, indent=2) + "\n", encoding="utf-8")
        return 0
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired, RuntimeError) as error:
        print(f"demo: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
