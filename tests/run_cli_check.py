#!/usr/bin/env python3
"""Check CLI discovery and file errors without running the large language suite."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rung", required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()

    def run(*flags):
        return subprocess.run([args.rung, *map(str, flags)], capture_output=True,
                              text=True, timeout=15)

    for flag in ("--help", "-h"):
        result = run(flag)
        assert result.returncode == 0 and not result.stderr, result
        assert "--engine=tree|stack|register" in result.stdout, result.stdout
    result = run("--version")
    assert result.returncode == 0 and not result.stderr, result
    assert result.stdout.startswith(f"rung {args.version}\nengines: tree stack register"), result
    assert run("--not-an-option").returncode == 64
    assert run().returncode == 64

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        empty = root / "empty.rg"
        empty.write_text("")
        result = run(empty)
        assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result
        valid = root / "program with spaces.rg"
        # Exercise both full read buffers and the final partial buffer.
        valid.write_text("// padding\n" * 2000 + "print 42;\n")
        result = run(valid)
        assert (result.returncode, result.stdout, result.stderr) == (0, "42\n", ""), result
        link = root / "link.rg"
        link.symlink_to(valid)
        assert run(link).stdout == "42\n"
        dangling = root / "dangling.rg"
        dangling.symlink_to(root / "missing.rg")
        for path in (root, root / "missing.rg", dangling):
            result = run(path)
            assert result.returncode == 66 and not result.stdout, result
            assert "cannot read" in result.stderr, result
    print("CLI help, version, file reads, and input errors passed")


if __name__ == "__main__":
    main()
