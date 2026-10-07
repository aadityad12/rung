#!/usr/bin/env bash
# Runs the front-end fuzzer for a fixed time (issue #12).
#
#   fuzz/run_fuzz.sh <path-to-rung_fuzz_frontend> [seconds]     (default: 60)
#
# Seeds are every tests/conformance/**/*.rg and examples/*.rg. libFuzzer reads a corpus
# directory flat, so they are copied into one scratch directory first. New interesting inputs
# go to a separate working corpus so the checked-in seeds are never modified. A crash, hang,
# leak or out-of-memory input is written to fuzz-artifacts/ (CI uploads that directory) and
# the script exits non-zero.
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <path-to-rung_fuzz_frontend> [seconds]" >&2
    exit 64
fi
fuzzer=$1
seconds=${2:-60}

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/seeds" "$work/corpus"
artifacts="$root/fuzz-artifacts"
mkdir -p "$artifacts"

# Flatten the seed files; the path is encoded in the name so two files never collide.
count=0
while IFS= read -r -d '' file; do
    rel=${file#"$root"/}
    cp "$file" "$work/seeds/${rel//\//__}"
    count=$((count + 1))
done < <(find "$root/tests" "$root/examples" -name '*.rg' -type f -print0 2>/dev/null)
echo "fuzz: $count seed files, running for ${seconds}s"

# -timeout: a single input taking longer than 10 s counts as a hang.
# There is deliberately no -max_len: the chain limit (docs/notes.md §2.6) means a long flat
# expression is a compile error rather than a stack overflow, so inputs need no size cap.
"$fuzzer" \
    -max_total_time="$seconds" \
    -timeout=10 \
    -dict="$root/fuzz/rung.dict" \
    -artifact_prefix="$artifacts/" \
    "$work/corpus" "$work/seeds"
