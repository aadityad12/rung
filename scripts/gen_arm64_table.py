#!/usr/bin/env python3
"""Generate tests/unit/arm64_encodings.inc: one {"asm text", 0xWORD} row per instruction.

The rows are the ground truth for src/jit/arm64_emitter: the unit tests assert that the emitter
produces the same 32-bit word as the LLVM assembler for every line below. The table is committed,
so building and testing never needs llvm-mc; run this script only when the case list changes:

    python3 scripts/gen_arm64_table.py

Assembler: `llvm-mc -triple=aarch64 -show-encoding` when it is on PATH (or in Homebrew's llvm),
otherwise the LLVM assembler built into Apple clang (`clang -c`, words read back with otool).
Both are the same LLVM AArch64 assembler; only the way we read the bytes out differs.

Every string here must appear, spelled identically, in tests/unit/arm64_emitter_test.cpp; the
test fails if a row is never exercised, so the two lists cannot drift apart.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "unit", "arm64_encodings.inc")

# Condition names, in encoding order except the aliases we also spell (hs/lo).
CONDS = ["eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"]
ALL_CONDS = CONDS + ["al"]


def cases():
    c = []

    def add(*lines):
        c.extend(lines)

    # arithmetic, register form
    for op in ["add", "sub", "adds", "subs"]:
        add(f"{op} w0, w1, w2", f"{op} x0, x1, x2", f"{op} x30, x29, x28", f"{op} w9, wzr, w10",
            f"{op} x9, xzr, x10", f"{op} w28, w29, wzr")
    add("cmp w0, w1", "cmp x0, x1", "cmp x30, xzr", "cmp wzr, w1")
    add("neg w0, w1", "neg x0, x1", "neg x30, x29", "neg w0, wzr")
    # arithmetic, immediate form
    add("add w0, w1, #0", "add w0, w1, #4095", "add x0, x1, #1", "add x0, x1, #4095",
        "add x0, x1, #4095, lsl #12", "add w0, w1, #1, lsl #12", "add sp, sp, #16",
        "add x0, sp, #0", "add wsp, wsp, #1", "add x29, sp, #4088")
    add("sub w0, w1, #4095", "sub x0, x1, #5", "sub sp, sp, #16", "sub sp, sp, #4095",
        "sub x0, x1, #1, lsl #12", "sub w30, wsp, #7", "sub x30, x29, #0")
    add("adds w0, w1, #7", "adds x0, x1, #4095", "adds x0, sp, #1", "adds w0, w1, #1, lsl #12",
        "subs w0, w1, #7", "subs x0, x1, #4095", "subs x0, sp, #4095",
        "subs x0, x1, #4095, lsl #12")
    add("cmp w0, #0", "cmp x0, #4095", "cmp sp, #16", "cmp w0, #1, lsl #12", "cmp x30, #1")
    # multiply, divide
    add("mul w0, w1, w2", "mul x0, x1, x2", "mul x30, x29, x28", "mul w0, wzr, w2")
    add("madd w0, w1, w2, w3", "madd x0, x1, x2, x3", "madd x30, x29, x28, x27",
        "madd w0, w1, w2, wzr")
    add("msub w0, w1, w2, w3", "msub x0, x1, x2, x3", "msub x30, x29, x28, x27",
        "msub x0, x1, x2, xzr")
    add("sdiv w0, w1, w2", "sdiv x0, x1, x2", "sdiv x30, x29, x28", "sdiv w0, wzr, w2")
    # logic
    for op in ["and", "orr", "eor"]:
        add(f"{op} w0, w1, w2", f"{op} x0, x1, x2", f"{op} x30, x29, x28", f"{op} w5, wzr, w6",
            f"{op} x5, x6, xzr")
    add("tst w0, w1", "tst x0, x1", "tst wzr, w1", "tst x30, x29")
    # immediate shifts
    for op in ["lsl", "lsr", "asr"]:
        add(f"{op} w0, w1, #0", f"{op} w0, w1, #1", f"{op} w30, w29, #31",
            f"{op} x0, x1, #0", f"{op} x0, x1, #1", f"{op} x30, x29, #63", f"{op} x2, xzr, #32")
    # move wide
    add("movz w0, #0", "movz w0, #65535", "movz w0, #1, lsl #16", "movz w30, #65535, lsl #16",
        "movz x0, #0", "movz x0, #65535", "movz x0, #2, lsl #16", "movz x0, #1, lsl #32",
        "movz x0, #65535, lsl #48", "movz x30, #0x1234, lsl #48")
    add("movn w0, #0", "movn w0, #5, lsl #16", "movn x0, #0", "movn x0, #65535",
        "movn x0, #65535, lsl #48", "movn x30, #1, lsl #32")
    add("movk w0, #1", "movk w0, #65535, lsl #16", "movk x0, #1", "movk x1, #7, lsl #32",
        "movk x0, #0xabcd, lsl #48", "movk x2, #9, lsl #16")
    # mov_imm helper: the exact sequences it must emit
    add("movz x0, #0xdef0", "movk x0, #0x9abc, lsl #16", "movk x0, #0x5678, lsl #32",
        "movk x0, #0x1234, lsl #48", "movn x0, #0xedcb", "movn x0, #0xffff, lsl #32",
        "movz x0, #0xffff, lsl #16", "movn x0, #0xa987", "movk x0, #0x1234, lsl #32",
        "movz w0, #0x5678", "movk w0, #0x1234, lsl #16", "movn w0, #0xedcb",
        "movz x1, #0xffff, lsl #48")
    # register move
    add("mov w0, w1", "mov x0, x1", "mov x30, x29", "mov x0, xzr", "mov w0, wzr", "mov x0, sp",
        "mov sp, x0", "mov w0, wsp", "mov wsp, w1")
    # load/store, unsigned scaled immediate
    for op in ["ldr", "str"]:
        add(f"{op} w0, [x1]", f"{op} w0, [x1, #4]", f"{op} w0, [x1, #16380]",
            f"{op} w30, [sp]", f"{op} wzr, [x0, #8]", f"{op} x0, [x1]", f"{op} x0, [x1, #8]",
            f"{op} x0, [x1, #32760]", f"{op} x0, [sp, #16]", f"{op} xzr, [sp, #32760]")
    # pair load/store with writeback
    add("ldp x29, x30, [sp], #16", "ldp x29, x30, [sp], #504", "ldp x29, x30, [sp], #-512",
        "ldp x19, x20, [sp], #32", "ldp w0, w1, [sp], #8", "ldp w0, w1, [x2], #252",
        "ldp w0, w1, [x2], #-256", "ldp x0, x1, [x2, #-16]!", "ldp x0, x1, [x2, #504]!",
        "ldp w0, w1, [x2, #-256]!")
    add("stp x29, x30, [sp, #-16]!", "stp x29, x30, [sp, #-512]!", "stp x29, x30, [sp, #504]!",
        "stp x19, x20, [sp, #-32]!", "stp w0, w1, [sp, #-256]!", "stp w0, w1, [sp, #252]!",
        "stp x29, x30, [sp], #16", "stp x0, x1, [x2], #-512", "stp w0, w1, [x2], #8",
        "stp xzr, xzr, [sp, #-16]!")
    # branches with raw byte offsets
    add("b #0", "b #4", "b #-4", "b #8", "b #12", "b #134217724", "b #-134217728",
        "bl #0", "bl #8", "bl #-4", "bl #-8", "bl #134217724", "bl #-134217728")
    for name in ALL_CONDS:
        add(f"b.{name} #8")
    add("b.eq #0", "b.ne #-4", "b.hs #1048572", "b.lo #-1048576", "b.gt #-8", "b.le #4")
    add("cbz w0, #8", "cbz x0, #-4", "cbz x30, #1048572", "cbz w0, #-1048576", "cbz xzr, #12",
        "cbnz w0, #8", "cbnz x0, #-4", "cbnz x0, #-8", "cbnz x0, #-12","cbnz x30, #1048572",
        "cbnz w0, #-1048576", "cbnz wzr, #12")
    add("blr x0", "blr x30", "blr x16", "br x0", "br x30", "br x16", "ret", "ret x1")
    for name in CONDS:
        add(f"cset w0, {name}", f"cset x30, {name}")
    add("cset w28, eq", "cset x1, lt")
    return c


def find_llvm_mc():
    for cand in [shutil.which("llvm-mc"), "/opt/homebrew/opt/llvm/bin/llvm-mc",
                 "/usr/local/opt/llvm/bin/llvm-mc"]:
        if cand and os.path.exists(cand):
            return cand
    return None


def assemble_llvm_mc(llvm_mc, lines):
    # One instruction per line -> one "encoding: [0x..,0x..,0x..,0x..]" per line, in order.
    proc = subprocess.run([llvm_mc, "-triple=aarch64", "-show-encoding"],
                          input="\n".join(lines) + "\n", capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"llvm-mc failed:\n{proc.stderr}")
    words = []
    for m in re.finditer(r"encoding: \[([^\]]*)\]", proc.stdout):
        b = [int(x, 16) for x in m.group(1).split(",")]
        if len(b) != 4:
            sys.exit(f"expected 4 bytes, got {m.group(1)}")
        words.append(b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24)
    return words


def assemble_clang(lines):
    clang = shutil.which("clang")
    otool = shutil.which("otool")
    if not clang or not otool:
        sys.exit("need llvm-mc, or Apple clang + otool, on PATH")
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "cases.s")
        obj = os.path.join(tmp, "cases.o")
        with open(src, "w") as f:
            f.write(".text\n" + "\n".join(lines) + "\n")
        proc = subprocess.run([clang, "-target", "arm64-apple-macos", "-c", src, "-o", obj],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            sys.exit(f"clang failed:\n{proc.stderr}")
        dump = subprocess.run([otool, "-t", obj], capture_output=True, text=True, check=True)
    words = []
    for line in dump.stdout.splitlines():
        parts = line.split()
        # "0000000000000000\t0b020020 ..." (address, then 32-bit words)
        if parts and re.fullmatch(r"[0-9a-f]{16}", parts[0]):
            words.extend(int(p, 16) for p in parts[1:])
    return words


def main():
    lines = cases()
    if len(set(lines)) != len(lines):
        dupes = sorted({x for x in lines if lines.count(x) > 1})
        sys.exit(f"duplicate case lines: {dupes}")
    llvm_mc = find_llvm_mc()
    words = assemble_llvm_mc(llvm_mc, lines) if llvm_mc else assemble_clang(lines)
    if len(words) != len(lines):
        sys.exit(f"assembled {len(words)} words for {len(lines)} lines (each line must be "
                 f"exactly one instruction)")
    with open(OUT, "w") as f:
        f.write("// Generated by scripts/gen_arm64_table.py from the LLVM assembler. "
                "Do not edit.\n")
        for line, w in zip(lines, words):
            f.write(f'{{"{line}", 0x{w:08X}}},\n')
    print(f"wrote {len(lines)} rows to {OUT} using {llvm_mc or 'clang + otool'}")


if __name__ == "__main__":
    main()
