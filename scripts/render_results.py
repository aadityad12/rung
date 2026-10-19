#!/usr/bin/env python3
"""Generate the documentation's SVG comparisons from committed results; --check never writes files."""
import argparse
from html import escape
import json
from pathlib import Path
import sys

from ladder import load_not_jit_compiled

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "docs/assets/performance.svg"


def render():
    configs = [("02_stack", "Stack VM", "#4e7fa7"),
               ("05_register", "Register VM", "#199b96"),
               ("09_jit", "JIT configuration", "#7966c7")]
    load = lambda name: json.loads((ROOT / "results" / f"{name}.json").read_text())
    baseline = load("01_tree")
    data = {name: load(name) for name, _, _ in configs}
    no_jit = load_not_jit_compiled(ROOT, data["09_jit"])
    benchmarks = list(baseline["benchmarks"])
    height = 190 + 86 * len(benchmarks)
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="960" height="{height}" '
           f'viewBox="0 0 960 {height}" role="img" aria-labelledby="title desc">',
           '<title id="title">Speedup over the tree-walker across six benchmarks</title>',
           '<desc id="desc">Stack VM, register VM and JIT configurations. Native compilation in '
           + escape(', '.join(b for b in benchmarks if b not in no_jit))
           + '; outlined bars mean VM fallback. Higher is faster.</desc>',
           f'<rect width="960" height="{height}" rx="16" fill="#f5f8fc"/>',
           '<g font-family="system-ui, sans-serif" fill="#172235">',
           '<text x="30" y="38" font-size="23" font-weight="700">What each engine buys you</text>',
           '<text x="30" y="65" font-size="14">Speedup over tree-walking · higher is faster · same six programs</text>']
    for i, (_, label, color) in enumerate(configs):
        x = 30 + i * 220
        svg += [f'<rect x="{x}" y="85" width="14" height="14" rx="3" fill="{color}"/>',
                f'<text x="{x+22}" y="97" font-size="14">{label}</text>']
    svg.append('<text x="690" y="97" font-size="13">Outlined = VM fallback</text>')
    start, scale = 155, 28
    def per_call(entry):
        return entry["median_total_ns"] / entry["iterations"]
    max_ratio = max(per_call(baseline["benchmarks"][b]) / per_call(data[c]["benchmarks"][b])
                    for b in benchmarks for c, _, _ in configs)
    # Scale to the available space rather than assuming future results stay below 25x.
    limit = max(5, int(max_ratio / 5 + 1) * 5)
    scale = 700 / limit
    for tick in range(0, limit + 1, max(1, limit // 5)):
        x = start + tick * scale
        svg += [f'<path d="M{x:.1f} 128 V{height-80}" stroke="#dce4ef"/>',
                f'<text x="{x:.1f}" y="122" text-anchor="middle" font-size="12">{tick}×</text>']
    for row, benchmark in enumerate(benchmarks):
        y = 135 + row * 86
        svg.append(f'<text x="30" y="{y+34}" font-size="15" font-weight="600">{escape(benchmark)}</text>')
        for i, (config, _, color) in enumerate(configs):
            ratio = per_call(baseline["benchmarks"][benchmark]) / per_call(data[config]["benchmarks"][benchmark])
            width = ratio * scale
            fallback = config == "09_jit" and benchmark in no_jit
            fill = "#f5f8fc" if fallback else color
            dash = ' stroke-dasharray="4 3"' if fallback else ''
            svg += [f'<rect x="{start}" y="{y+i*21}" width="{width:.2f}" height="14" '
                    f'rx="3" fill="{fill}" stroke="{color}"{dash}/>',
                    f'<text x="{start+width+7:.2f}" y="{y+i*21+12}" font-size="12">{ratio:.2f}×</text>']
    machine = escape(f"{baseline['meta']['cpu']} · {baseline['meta']['os']}")
    svg += [f'<text x="30" y="{height-48}" font-size="13">{machine} · results/*.json</text>',
            f'<text x="30" y="{height-26}" font-size="12">Median process total / calls. Sessions differ; see methodology and full tables.</text>',
            '</g></svg>']
    return '\n'.join(svg) + '\n'


def render_jit():
    load = lambda name: json.loads((ROOT / "results" / f"{name}.json").read_text())
    baseline, jit = load("08_fold"), load("09_jit")
    for field in ("date", "commit", "cpu", "os", "iterations", "kept_runs", "seed"):
        if baseline["meta"][field] != jit["meta"][field]:
            raise ValueError(f"JIT comparison requires matching session metadata: {field}")
    benchmarks = list(load("01_tree")["benchmarks"])
    no_jit = load_not_jit_compiled(ROOT, jit)
    per_call = lambda entry: entry["median_total_ns"] / entry["iterations"] / 1e6
    ratios = {b: per_call(baseline["benchmarks"][b]) / per_call(jit["benchmarks"][b])
              for b in benchmarks}
    limit = max(2, int(max(ratios.values())) + 1)
    start, scale = 150, 450 / limit
    height = 230 + 55 * len(benchmarks)
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="960" height="{height}" '
           f'viewBox="0 0 960 {height}" role="img" aria-labelledby="title desc">',
           '<title id="title">JIT versus the optimized register VM, measured together</title>',
           '<desc id="desc">Speedup on a shared linear scale. One times means equal speed. '
           'Dashed bars indicate VM fallback. Rounded timings appear alongside each bar.</desc>',
           f'<rect width="960" height="{height}" rx="16" fill="#f5f8fc"/>',
           '<g font-family="system-ui, sans-serif" fill="#172235">',
           '<text x="30" y="38" font-size="23" font-weight="700">Where native compilation helps</text>',
           '<text x="30" y="65" font-size="14">08_fold → 09_jit · same source and measurement session · higher is faster</text>',
           '<text x="30" y="92" font-size="13">Solid = native compilation · dashed = VM fallback</text>',
           '<text x="700" y="120" font-size="13">VM → JIT (ms/call)</text>']
    for tick in range(limit + 1):
        x = start + tick * scale
        svg += [f'<path d="M{x:.1f} 135 V{height-115}" stroke="{ "#8494aa" if tick == 1 else "#dce4ef" }"/>',
                f'<text x="{x:.1f}" y="122" text-anchor="middle" font-size="12">{tick}×</text>']
    for row, b in enumerate(benchmarks):
        y = 148 + row * 55
        ratio = ratios[b]
        fallback = b in no_jit
        fill = "#f5f8fc" if fallback else "#7966c7"
        dash = ' stroke-dasharray="4 3"' if fallback else ''
        svg += [f'<text x="30" y="{y+18}" font-size="15" font-weight="600">{escape(b)}</text>',
                f'<rect x="{start}" y="{y}" width="{ratio*scale:.2f}" height="25" rx="3" fill="{fill}" stroke="#7966c7"{dash}/>',
                f'<text x="{start+ratio*scale+8:.2f}" y="{y+18}" font-size="14">{ratio:.2f}×</text>',
                f'<text x="700" y="{y+18}" font-size="14">{per_call(baseline["benchmarks"][b]):.2f} → {per_call(jit["benchmarks"][b]):.2f}</text>']
    meta = jit["meta"]
    svg += [f'<text x="30" y="{height-67}" font-size="13">{escape(meta["cpu"])} · {escape(meta["date"][:10])} · source {escape(meta["commit"][:7])}</text>',
            f'<text x="30" y="{height-44}" font-size="12">Median process total / calls, including initial interpreted work. Not pooled p50.</text>',
            f'<text x="30" y="{height-23}" font-size="12">Source: results/08_fold.json and results/09_jit.json. Near-parity differences include small regressions.</text>',
            '</g></svg>']
    return '\n'.join(svg) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    outputs = {DEST: render(), DEST.with_name("jit-comparison.svg"): render_jit()}
    for dest, svg in outputs.items():
        if args.check:
            if not dest.exists() or dest.read_text() != svg:
                print(f"{dest.name} is stale; run python3 scripts/render_results.py", file=sys.stderr)
                return 1
            print(f"{dest.name} matches committed results")
        else:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(svg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
