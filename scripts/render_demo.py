#!/usr/bin/env python3
"""Render the checked demo capture into terminal GIFs, stills, and a text transcript.

Requires Pillow only for rendering. Running Rung or its guided demo needs no Pillow.
"""
import argparse
import hashlib
import json
from pathlib import Path
import textwrap

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 1000, 550
BACKGROUND, PANEL = "#101827", "#172235"
TEXT, MUTED, ACCENT, GREEN = "#edf3fb", "#aabbd1", "#79d8ee", "#a6e3ad"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, default=ROOT / "docs/assets/demo.json")
    parser.add_argument("--output", type=Path, default=ROOT / "docs/assets")
    parser.add_argument("--font", type=Path, help="path to a monospace TrueType/OpenType font")
    args = parser.parse_args()
    candidates = [args.font, Path("/System/Library/Fonts/Menlo.ttc"),
                  Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")]
    font_path = next((p for p in candidates if p and p.is_file()), None)
    if font_path is None:
        parser.error("pass --font with a monospace font file")
    font = ImageFont.truetype(str(font_path), 22)
    small = ImageFont.truetype(str(font_path), 17)
    title_font = ImageFont.truetype(str(font_path), 25)
    capture = json.loads(args.capture.read_text())
    for name, expected in capture["example_sha256"].items():
        actual = hashlib.sha256((ROOT / "examples" / name).read_bytes()).hexdigest()
        if actual != expected:
            parser.error(f"{name} changed; recapture the demo before rendering")
    args.output.mkdir(parents=True, exist_ok=True)

    def frame(section, title, caption, lines, progress):
        canvas = Image.new("RGB", (WIDTH, HEIGHT), BACKGROUND)
        draw = ImageDraw.Draw(canvas)
        draw.rounded_rectangle((16, 16, WIDTH - 16, HEIGHT - 16), radius=16, fill=PANEL)
        for x, color in ((42, "#f38b8b"), (64, "#f9d58b"), (86, GREEN)):
            draw.ellipse((x, 38, x + 10, 48), fill=color)
        draw.text((116, 32), f"RUNG / {section.upper()}", font=small, fill=MUTED)
        draw.text((38, 79), title, font=title_font, fill=TEXT)
        for row, line in enumerate(textwrap.wrap(caption, 86)):
            draw.text((38, 118 + row * 22), line, font=small, fill=MUTED)
        y = 178
        for line, color in lines:
            # Commands are wrapped at spaces; bytecode and output fit without reformatting.
            wrapped = textwrap.wrap(line, 68, subsequent_indent="  ",
                                    replace_whitespace=False, drop_whitespace=False) or [""]
            for part in wrapped:
                if y > 464:
                    raise ValueError(f"terminal frame overflow: {title}")
                draw.text((38, y), part, font=font, fill=color)
                y += 28
        draw.line((38, 491, WIDTH - 38, 491), fill="#30405a", width=1)
        draw.line((38, 491, 38 + int((WIDTH - 76) * progress), 491), fill=ACCENT, width=2)
        draw.text((38, 508), "Real output / paced playback / compile times and code sizes omitted",
                  font=small, fill=MUTED)
        return canvas

    transcript = ["# Demo transcript", "", capture["presentation"], "",
                  f"Captured with {capture['version'].splitlines()[0]} on "
                  f"{capture['platform']} {capture['architecture']}.", "",
                  "Commands use `rung` for readability; point them at your built or installed binary.", ""]
    for section in ("engines", "bytecode", "jit", "recursion"):
        items = [r for r in capture["records"] if r["section"] == section]
        if not items:
            parser.error(f"capture has no {section} demo; record with a native ARM64 JIT build")
        frames, durations = [], []
        for i, item in enumerate(items):
            command = [("$ " + item["command"], ACCENT)]
            frames.append(frame(section, item["title"], item["caption"], command, i / len(items)))
            durations.append(1100)
            output = []
            if item["stderr"]:
                output += [("stderr:", MUTED)] + [(s, TEXT) for s in item["stderr"].splitlines()]
            if item["stdout"]:
                if item["stderr"]:
                    output.append(("stdout:", MUTED))
                output += [(s, GREEN if section != "bytecode" else TEXT)
                           for s in item["stdout"].splitlines()]
            # Long command lines and logs get their own frame, keeping terminal text legible.
            lines = output if section in ("jit", "recursion") else command + [("", MUTED)] + output
            frames.append(frame(section, item["title"], item["caption"], lines, (i + 1) / len(items)))
            durations.append(3700 if section != "engines" else 1800)
            transcript += [f"## {item['title']}", "", item["caption"], "", "```sh",
                           item["command"], "```", ""]
            for stream in ("stderr", "stdout"):
                if item[stream]:
                    transcript += [f"{stream}:", "", "```text", item[stream].rstrip(), "```", ""]
        if section == "engines":
            summary = [(f"{r['title']:<20} {' / '.join(r['stdout'].splitlines())}", GREEN)
                       for r in items]
            summary += [("", MUTED), ("PASS: every available configuration agrees.", TEXT)]
            frames.append(frame(section, "Same language. Same answers.",
                                "examples/hot_loop.rg — two calls per configuration", summary, 1))
            durations.append(4500)
        frames[-1].save(args.output / f"{section}.png")
        # A shared palette keeps the terminal colors stable between animation frames.
        palette = frames[-1].quantize(colors=64)
        indexed = [f.quantize(palette=palette, dither=Image.Dither.NONE) for f in frames]
        indexed[0].save(args.output / f"{section}.gif", save_all=True,
                        append_images=indexed[1:], duration=durations, loop=0, optimize=True,
                        disposal=2)
        print(f"{section}.gif: {(args.output / (section + '.gif')).stat().st_size:,} bytes")
    (args.output / "demo-transcript.md").write_text("\n".join(transcript), encoding="utf-8")


if __name__ == "__main__":
    main()
