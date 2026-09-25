// Byte-exact tests for the ARM64 encoder. Every row of arm64_encodings.inc was produced by the
// LLVM assembler (scripts/gen_arm64_table.py); the emitter must produce the identical 32-bit
// word. The table is committed, so these tests need no assembler installed.
#include <doctest.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "jit/arm64_emitter.h"

using namespace rung::arm64;

namespace {

const std::map<std::string, uint32_t>& table() {
    static const std::map<std::string, uint32_t> rows = {
#include "arm64_encodings.inc"
    };
    return rows;
}

std::set<std::string>& used_rows() {
    static std::set<std::string> used;
    return used;
}

uint32_t expected(const std::string& text) {
    const auto it = table().find(text);
    if (it == table().end()) {
        FAIL("no table row for: " << text);
    }
    used_rows().insert(text);
    return it->second;
}

// The emitter, run through `emit_fn`, must produce exactly the words of the given table rows.
template <class F>
void check_seq(const std::vector<std::string>& texts, F emit_fn) {
    Emitter e;
    emit_fn(e);
    e.finish();
    REQUIRE(e.code().size() == texts.size());
    for (size_t i = 0; i < texts.size(); ++i) {
        INFO(texts[i]);
        CHECK(e.code()[i] == expected(texts[i]));
    }
}

template <class F>
void check_one(const std::string& text, F emit_fn) {
    check_seq({text}, emit_fn);
}

#define CASE(text, call) check_one(text, [](Emitter& e) { e.call; })

struct CondName {
    const char* name;
    Cond cond;
};
const CondName kConds[] = {
    {"eq", Cond::EQ}, {"ne", Cond::NE}, {"hs", Cond::HS}, {"lo", Cond::LO}, {"mi", Cond::MI},
    {"pl", Cond::PL}, {"vs", Cond::VS}, {"vc", Cond::VC}, {"hi", Cond::HI}, {"ls", Cond::LS},
    {"ge", Cond::GE}, {"lt", Cond::LT}, {"gt", Cond::GT}, {"le", Cond::LE},
};

}  // namespace

TEST_CASE("arm64: every instruction matches the assembler, word for word") {
    // ---- arithmetic, register form
    CASE("add w0, w1, w2", add(W0, W1, W2));
    CASE("add x0, x1, x2", add(X0, X1, X2));
    CASE("add x30, x29, x28", add(X30, X29, X28));
    CASE("add w9, wzr, w10", add(W9, WZR, W10));
    CASE("add x9, xzr, x10", add(X9, XZR, X10));
    CASE("add w28, w29, wzr", add(W28, W29, WZR));
    CASE("sub w0, w1, w2", sub(W0, W1, W2));
    CASE("sub x0, x1, x2", sub(X0, X1, X2));
    CASE("sub x30, x29, x28", sub(X30, X29, X28));
    CASE("sub w9, wzr, w10", sub(W9, WZR, W10));
    CASE("sub x9, xzr, x10", sub(X9, XZR, X10));
    CASE("sub w28, w29, wzr", sub(W28, W29, WZR));
    CASE("adds w0, w1, w2", adds(W0, W1, W2));
    CASE("adds x0, x1, x2", adds(X0, X1, X2));
    CASE("adds x30, x29, x28", adds(X30, X29, X28));
    CASE("adds w9, wzr, w10", adds(W9, WZR, W10));
    CASE("adds x9, xzr, x10", adds(X9, XZR, X10));
    CASE("adds w28, w29, wzr", adds(W28, W29, WZR));
    CASE("subs w0, w1, w2", subs(W0, W1, W2));
    CASE("subs x0, x1, x2", subs(X0, X1, X2));
    CASE("subs x30, x29, x28", subs(X30, X29, X28));
    CASE("subs w9, wzr, w10", subs(W9, WZR, W10));
    CASE("subs x9, xzr, x10", subs(X9, XZR, X10));
    CASE("subs w28, w29, wzr", subs(W28, W29, WZR));
    CASE("cmp w0, w1", cmp(W0, W1));
    CASE("cmp x0, x1", cmp(X0, X1));
    CASE("cmp x30, xzr", cmp(X30, XZR));
    CASE("cmp wzr, w1", cmp(WZR, W1));
    CASE("neg w0, w1", neg(W0, W1));
    CASE("neg x0, x1", neg(X0, X1));
    CASE("neg x30, x29", neg(X30, X29));
    CASE("neg w0, wzr", neg(W0, WZR));

    // ---- arithmetic, immediate form (max immediates, sp, lsl #12)
    CASE("add w0, w1, #0", add_imm(W0, W1, 0));
    CASE("add w0, w1, #4095", add_imm(W0, W1, 4095));
    CASE("add x0, x1, #1", add_imm(X0, X1, 1));
    CASE("add x0, x1, #4095", add_imm(X0, X1, 4095));
    CASE("add x0, x1, #4095, lsl #12", add_imm(X0, X1, 4095, true));
    CASE("add w0, w1, #1, lsl #12", add_imm(W0, W1, 1, true));
    CASE("add sp, sp, #16", add_imm(SP, SP, 16));
    CASE("add x0, sp, #0", add_imm(X0, SP, 0));
    CASE("add wsp, wsp, #1", add_imm(WSP, WSP, 1));
    CASE("add x29, sp, #4088", add_imm(X29, SP, 4088));
    CASE("sub w0, w1, #4095", sub_imm(W0, W1, 4095));
    CASE("sub x0, x1, #5", sub_imm(X0, X1, 5));
    CASE("sub sp, sp, #16", sub_imm(SP, SP, 16));
    CASE("sub sp, sp, #4095", sub_imm(SP, SP, 4095));
    CASE("sub x0, x1, #1, lsl #12", sub_imm(X0, X1, 1, true));
    CASE("sub w30, wsp, #7", sub_imm(W30, WSP, 7));
    CASE("sub x30, x29, #0", sub_imm(X30, X29, 0));
    CASE("adds w0, w1, #7", adds_imm(W0, W1, 7));
    CASE("adds x0, x1, #4095", adds_imm(X0, X1, 4095));
    CASE("adds x0, sp, #1", adds_imm(X0, SP, 1));
    CASE("adds w0, w1, #1, lsl #12", adds_imm(W0, W1, 1, true));
    CASE("subs w0, w1, #7", subs_imm(W0, W1, 7));
    CASE("subs x0, x1, #4095", subs_imm(X0, X1, 4095));
    CASE("subs x0, sp, #4095", subs_imm(X0, SP, 4095));
    CASE("subs x0, x1, #4095, lsl #12", subs_imm(X0, X1, 4095, true));
    CASE("cmp w0, #0", cmp_imm(W0, 0));
    CASE("cmp x0, #4095", cmp_imm(X0, 4095));
    CASE("cmp sp, #16", cmp_imm(SP, 16));
    CASE("cmp w0, #1, lsl #12", cmp_imm(W0, 1, true));
    CASE("cmp x30, #1", cmp_imm(X30, 1));

    // ---- multiply, divide
    CASE("mul w0, w1, w2", mul(W0, W1, W2));
    CASE("mul x0, x1, x2", mul(X0, X1, X2));
    CASE("mul x30, x29, x28", mul(X30, X29, X28));
    CASE("mul w0, wzr, w2", mul(W0, WZR, W2));
    CASE("madd w0, w1, w2, w3", madd(W0, W1, W2, W3));
    CASE("madd x0, x1, x2, x3", madd(X0, X1, X2, X3));
    CASE("madd x30, x29, x28, x27", madd(X30, X29, X28, X27));
    CASE("madd w0, w1, w2, wzr", madd(W0, W1, W2, WZR));
    CASE("msub w0, w1, w2, w3", msub(W0, W1, W2, W3));
    CASE("msub x0, x1, x2, x3", msub(X0, X1, X2, X3));
    CASE("msub x30, x29, x28, x27", msub(X30, X29, X28, X27));
    CASE("msub x0, x1, x2, xzr", msub(X0, X1, X2, XZR));
    CASE("sdiv w0, w1, w2", sdiv(W0, W1, W2));
    CASE("sdiv x0, x1, x2", sdiv(X0, X1, X2));
    CASE("sdiv x30, x29, x28", sdiv(X30, X29, X28));
    CASE("sdiv w0, wzr, w2", sdiv(W0, WZR, W2));

    // ---- logic
    CASE("and w0, w1, w2", and_(W0, W1, W2));
    CASE("and x0, x1, x2", and_(X0, X1, X2));
    CASE("and x30, x29, x28", and_(X30, X29, X28));
    CASE("and w5, wzr, w6", and_(W5, WZR, W6));
    CASE("and x5, x6, xzr", and_(X5, X6, XZR));
    CASE("orr w0, w1, w2", orr(W0, W1, W2));
    CASE("orr x0, x1, x2", orr(X0, X1, X2));
    CASE("orr x30, x29, x28", orr(X30, X29, X28));
    CASE("orr w5, wzr, w6", orr(W5, WZR, W6));
    CASE("orr x5, x6, xzr", orr(X5, X6, XZR));
    CASE("eor w0, w1, w2", eor(W0, W1, W2));
    CASE("eor x0, x1, x2", eor(X0, X1, X2));
    CASE("eor x30, x29, x28", eor(X30, X29, X28));
    CASE("eor w5, wzr, w6", eor(W5, WZR, W6));
    CASE("eor x5, x6, xzr", eor(X5, X6, XZR));
    CASE("tst w0, w1", tst(W0, W1));
    CASE("tst x0, x1", tst(X0, X1));
    CASE("tst wzr, w1", tst(WZR, W1));
    CASE("tst x30, x29", tst(X30, X29));

    // ---- immediate shifts (0, 1 and the maximum for each width)
    CASE("lsl w0, w1, #0", lsl(W0, W1, 0));
    CASE("lsl w0, w1, #1", lsl(W0, W1, 1));
    CASE("lsl w30, w29, #31", lsl(W30, W29, 31));
    CASE("lsl x0, x1, #0", lsl(X0, X1, 0));
    CASE("lsl x0, x1, #1", lsl(X0, X1, 1));
    CASE("lsl x30, x29, #63", lsl(X30, X29, 63));
    CASE("lsl x2, xzr, #32", lsl(X2, XZR, 32));
    CASE("lsr w0, w1, #0", lsr(W0, W1, 0));
    CASE("lsr w0, w1, #1", lsr(W0, W1, 1));
    CASE("lsr w30, w29, #31", lsr(W30, W29, 31));
    CASE("lsr x0, x1, #0", lsr(X0, X1, 0));
    CASE("lsr x0, x1, #1", lsr(X0, X1, 1));
    CASE("lsr x30, x29, #63", lsr(X30, X29, 63));
    CASE("lsr x2, xzr, #32", lsr(X2, XZR, 32));
    CASE("asr w0, w1, #0", asr(W0, W1, 0));
    CASE("asr w0, w1, #1", asr(W0, W1, 1));
    CASE("asr w30, w29, #31", asr(W30, W29, 31));
    CASE("asr x0, x1, #0", asr(X0, X1, 0));
    CASE("asr x0, x1, #1", asr(X0, X1, 1));
    CASE("asr x30, x29, #63", asr(X30, X29, 63));
    CASE("asr x2, xzr, #32", asr(X2, XZR, 32));

    // ---- move wide
    CASE("movz w0, #0", movz(W0, 0));
    CASE("movz w0, #65535", movz(W0, 65535));
    CASE("movz w0, #1, lsl #16", movz(W0, 1, 16));
    CASE("movz w30, #65535, lsl #16", movz(W30, 65535, 16));
    CASE("movz x0, #0", movz(X0, 0));
    CASE("movz x0, #65535", movz(X0, 65535));
    CASE("movz x0, #2, lsl #16", movz(X0, 2, 16));
    CASE("movz x0, #1, lsl #32", movz(X0, 1, 32));
    CASE("movz x0, #65535, lsl #48", movz(X0, 65535, 48));
    CASE("movz x30, #0x1234, lsl #48", movz(X30, 0x1234, 48));
    CASE("movn w0, #0", movn(W0, 0));
    CASE("movn w0, #5, lsl #16", movn(W0, 5, 16));
    CASE("movn x0, #0", movn(X0, 0));
    CASE("movn x0, #65535", movn(X0, 65535));
    CASE("movn x0, #65535, lsl #48", movn(X0, 65535, 48));
    CASE("movn x30, #1, lsl #32", movn(X30, 1, 32));
    CASE("movk w0, #1", movk(W0, 1));
    CASE("movk w0, #65535, lsl #16", movk(W0, 65535, 16));
    CASE("movk x0, #1", movk(X0, 1));
    CASE("movk x1, #7, lsl #32", movk(X1, 7, 32));
    CASE("movk x0, #0xabcd, lsl #48", movk(X0, 0xabcd, 48));
    CASE("movk x2, #9, lsl #16", movk(X2, 9, 16));

    // ---- register move (ORR alias, or ADD #0 when sp is involved)
    CASE("mov w0, w1", mov(W0, W1));
    CASE("mov x0, x1", mov(X0, X1));
    CASE("mov x30, x29", mov(X30, X29));
    CASE("mov x0, xzr", mov(X0, XZR));
    CASE("mov w0, wzr", mov(W0, WZR));
    CASE("mov x0, sp", mov(X0, SP));
    CASE("mov sp, x0", mov(SP, X0));
    CASE("mov w0, wsp", mov(W0, WSP));
    CASE("mov wsp, w1", mov(WSP, W1));

    // ---- load/store, unsigned scaled immediate (offset 0, small, and the maximum)
    CASE("ldr w0, [x1]", ldr(W0, X1));
    CASE("ldr w0, [x1, #4]", ldr(W0, X1, 4));
    CASE("ldr w0, [x1, #16380]", ldr(W0, X1, 16380));
    CASE("ldr w30, [sp]", ldr(W30, SP));
    CASE("ldr wzr, [x0, #8]", ldr(WZR, X0, 8));
    CASE("ldr x0, [x1]", ldr(X0, X1));
    CASE("ldr x0, [x1, #8]", ldr(X0, X1, 8));
    CASE("ldr x0, [x1, #32760]", ldr(X0, X1, 32760));
    CASE("ldr x0, [sp, #16]", ldr(X0, SP, 16));
    CASE("ldr xzr, [sp, #32760]", ldr(XZR, SP, 32760));
    CASE("str w0, [x1]", str(W0, X1));
    CASE("str w0, [x1, #4]", str(W0, X1, 4));
    CASE("str w0, [x1, #16380]", str(W0, X1, 16380));
    CASE("str w30, [sp]", str(W30, SP));
    CASE("str wzr, [x0, #8]", str(WZR, X0, 8));
    CASE("str x0, [x1]", str(X0, X1));
    CASE("str x0, [x1, #8]", str(X0, X1, 8));
    CASE("str x0, [x1, #32760]", str(X0, X1, 32760));
    CASE("str x0, [sp, #16]", str(X0, SP, 16));
    CASE("str xzr, [sp, #32760]", str(XZR, SP, 32760));

    // ---- pair load/store with writeback (extreme offsets, sp base)
    CASE("ldp x29, x30, [sp], #16", ldp_post(X29, X30, SP, 16));
    CASE("ldp x29, x30, [sp], #504", ldp_post(X29, X30, SP, 504));
    CASE("ldp x29, x30, [sp], #-512", ldp_post(X29, X30, SP, -512));
    CASE("ldp x19, x20, [sp], #32", ldp_post(X19, X20, SP, 32));
    CASE("ldp w0, w1, [sp], #8", ldp_post(W0, W1, SP, 8));
    CASE("ldp w0, w1, [x2], #252", ldp_post(W0, W1, X2, 252));
    CASE("ldp w0, w1, [x2], #-256", ldp_post(W0, W1, X2, -256));
    CASE("ldp x0, x1, [x2, #-16]!", ldp_pre(X0, X1, X2, -16));
    CASE("ldp x0, x1, [x2, #504]!", ldp_pre(X0, X1, X2, 504));
    CASE("ldp w0, w1, [x2, #-256]!", ldp_pre(W0, W1, X2, -256));
    CASE("stp x29, x30, [sp, #-16]!", stp_pre(X29, X30, SP, -16));
    CASE("stp x29, x30, [sp, #-512]!", stp_pre(X29, X30, SP, -512));
    CASE("stp x29, x30, [sp, #504]!", stp_pre(X29, X30, SP, 504));
    CASE("stp x19, x20, [sp, #-32]!", stp_pre(X19, X20, SP, -32));
    CASE("stp w0, w1, [sp, #-256]!", stp_pre(W0, W1, SP, -256));
    CASE("stp w0, w1, [sp, #252]!", stp_pre(W0, W1, SP, 252));
    CASE("stp x29, x30, [sp], #16", stp_post(X29, X30, SP, 16));
    CASE("stp x0, x1, [x2], #-512", stp_post(X0, X1, X2, -512));
    CASE("stp w0, w1, [x2], #8", stp_post(W0, W1, X2, 8));
    CASE("stp xzr, xzr, [sp, #-16]!", stp_pre(XZR, XZR, SP, -16));

    // ---- branches with raw byte offsets (zero, +-4, and the extremes of each range)
    CASE("b #0", b_offset(0));
    CASE("b #4", b_offset(4));
    CASE("b #-4", b_offset(-4));
    CASE("b #8", b_offset(8));
    CASE("b #12", b_offset(12));
    CASE("b #134217724", b_offset(134217724));
    CASE("b #-134217728", b_offset(-134217728));
    CASE("bl #0", bl_offset(0));
    CASE("bl #8", bl_offset(8));
    CASE("bl #-4", bl_offset(-4));
    CASE("bl #-8", bl_offset(-8));
    CASE("bl #134217724", bl_offset(134217724));
    CASE("bl #-134217728", bl_offset(-134217728));
    CASE("b.eq #0", b_cond_offset(Cond::EQ, 0));
    CASE("b.ne #-4", b_cond_offset(Cond::NE, -4));
    CASE("b.hs #1048572", b_cond_offset(Cond::HS, 1048572));
    CASE("b.lo #-1048576", b_cond_offset(Cond::LO, -1048576));
    CASE("b.gt #-8", b_cond_offset(Cond::GT, -8));
    CASE("b.le #4", b_cond_offset(Cond::LE, 4));
    for (const CondName& c : kConds) {
        check_one(std::string("b.") + c.name + " #8",
                  [&](Emitter& e) { e.b_cond_offset(c.cond, 8); });
    }
    CASE("b.al #8", b_cond_offset(Cond::AL, 8));
    CASE("cbz w0, #8", cbz_offset(W0, 8));
    CASE("cbz x0, #-4", cbz_offset(X0, -4));
    CASE("cbz x30, #1048572", cbz_offset(X30, 1048572));
    CASE("cbz w0, #-1048576", cbz_offset(W0, -1048576));
    CASE("cbz xzr, #12", cbz_offset(XZR, 12));
    CASE("cbnz w0, #8", cbnz_offset(W0, 8));
    CASE("cbnz x0, #-4", cbnz_offset(X0, -4));
    CASE("cbnz x0, #-8", cbnz_offset(X0, -8));
    CASE("cbnz x0, #-12", cbnz_offset(X0, -12));
    CASE("cbnz x30, #1048572", cbnz_offset(X30, 1048572));
    CASE("cbnz w0, #-1048576", cbnz_offset(W0, -1048576));
    CASE("cbnz wzr, #12", cbnz_offset(WZR, 12));
    CASE("blr x0", blr(X0));
    CASE("blr x30", blr(X30));
    CASE("blr x16", blr(X16));
    CASE("br x0", br(X0));
    CASE("br x30", br(X30));
    CASE("br x16", br(X16));
    CASE("ret", ret());
    CASE("ret x1", ret(X1));

    // ---- cset, every condition except AL
    for (const CondName& c : kConds) {
        check_one(std::string("cset w0, ") + c.name, [&](Emitter& e) { e.cset(W0, c.cond); });
        check_one(std::string("cset x30, ") + c.name, [&](Emitter& e) { e.cset(X30, c.cond); });
    }
    CASE("cset w28, eq", cset(W28, Cond::EQ));
    CASE("cset x1, lt", cset(X1, Cond::LT));

    // ---- mov_imm: fewest instructions, MOVN when most chunks are all-ones
    check_one("movz x0, #0", [](Emitter& e) { e.mov_imm(X0, 0); });
    check_one("movn x0, #0", [](Emitter& e) { e.mov_imm(X0, ~uint64_t{0}); });
    check_one("movz x0, #65535", [](Emitter& e) { e.mov_imm(X0, 0xFFFF); });
    check_one("movz x0, #1, lsl #32", [](Emitter& e) { e.mov_imm(X0, uint64_t{1} << 32); });
    check_seq({"movz x0, #0xdef0", "movk x0, #0x9abc, lsl #16", "movk x0, #0x5678, lsl #32",
               "movk x0, #0x1234, lsl #48"},
              [](Emitter& e) { e.mov_imm(X0, 0x123456789abcdef0ull); });
    check_one("movn x0, #0xedcb", [](Emitter& e) { e.mov_imm(X0, 0xFFFFFFFFFFFF1234ull); });
    check_one("movn x0, #0xffff, lsl #32",
              [](Emitter& e) { e.mov_imm(X0, 0xFFFF0000FFFFFFFFull); });
    check_one("movz x0, #0xffff, lsl #16",
              [](Emitter& e) { e.mov_imm(X0, 0x00000000FFFF0000ull); });
    check_seq({"movn x0, #0xa987", "movk x0, #0x1234, lsl #32"},
              [](Emitter& e) { e.mov_imm(X0, 0xFFFF1234FFFF5678ull); });
    check_one("movz x1, #0xffff, lsl #48",
              [](Emitter& e) { e.mov_imm(X1, 0xFFFF000000000000ull); });
    check_one("movz w0, #0", [](Emitter& e) { e.mov_imm(W0, 0); });
    check_one("movn w0, #0", [](Emitter& e) { e.mov_imm(W0, 0xFFFFFFFFu); });
    check_seq({"movz w0, #0x5678", "movk w0, #0x1234, lsl #16"},
              [](Emitter& e) { e.mov_imm(W0, 0x12345678u); });
    check_one("movn w0, #0xedcb", [](Emitter& e) { e.mov_imm(W0, 0xFFFF1234u); });

    // Guard the test itself: a row nobody exercises means the two case lists drifted apart.
    std::vector<std::string> unused;
    for (const auto& row : table()) {
        if (used_rows().count(row.first) == 0) {
            unused.push_back(row.first);
        }
    }
    INFO("first unused row: " << (unused.empty() ? "" : unused.front()));
    CHECK(unused.empty());
}

// ---- labels and fixups ------------------------------------------------------------------------

TEST_CASE("arm64: forward branch is patched when the label is bound") {
    Emitter e;
    const Label l = e.make_label();
    e.b(l);                // index 0, target is index 3: offset +12
    e.mov(X0, X1);
    e.mov(X0, X1);
    e.bind(l);
    e.finish();
    CHECK(e.code()[0] == expected("b #12"));
}

TEST_CASE("arm64: several forward branches to one label are all patched") {
    Emitter e;
    const Label l = e.make_label();
    e.b(l);                        // +12
    e.b_cond(Cond::EQ, l);         // +8
    e.mov(X0, X1);
    e.bind(l);
    e.finish();
    CHECK(e.code()[0] == expected("b #12"));
    CHECK(e.code()[1] == expected("b.eq #8"));
}

TEST_CASE("arm64: forward cbz is patched") {
    Emitter e;
    const Label l = e.make_label();
    e.cbz(W0, l);
    e.mov(X0, X1);
    e.bind(l);
    CHECK(e.code()[0] == expected("cbz w0, #8"));
}

TEST_CASE("arm64: backward branches to an already-bound label get a negative offset") {
    Emitter e;
    const Label l = e.make_label();
    e.bind(l);
    e.mov(X0, X1);
    e.b_cond(Cond::NE, l);   // -4
    e.bl(l);                 // -8
    e.cbnz(X0, l);           // -12
    e.finish();
    CHECK(e.code()[1] == expected("b.ne #-4"));
    CHECK(e.code()[2] == expected("bl #-8"));
    CHECK(e.code()[3] == expected("cbnz x0, #-12"));
}

TEST_CASE("arm64: label branch to the next instruction is +4") {
    Emitter e;
    const Label l = e.make_label();
    e.b(l);
    e.bind(l);
    CHECK(e.code()[0] == expected("b #4"));
}

// ---- hard errors ------------------------------------------------------------------------------

TEST_CASE("arm64: out-of-range immediates and offsets are hard errors") {
    Emitter e;
    CHECK_THROWS_AS(e.add_imm(X0, X1, 4096), EmitError);
    CHECK_THROWS_AS(e.cmp_imm(W0, 4096), EmitError);
    CHECK_THROWS_AS(e.lsl(W0, W1, 32), EmitError);
    CHECK_THROWS_AS(e.lsr(X0, X1, 64), EmitError);
    CHECK_THROWS_AS(e.movz(X0, 65536), EmitError);
    CHECK_THROWS_AS(e.movz(W0, 1, 32), EmitError);
    CHECK_THROWS_AS(e.movz(X0, 1, 8), EmitError);
    CHECK_THROWS_AS(e.movz(X0, 1, 64), EmitError);
    CHECK_THROWS_AS(e.ldr(X0, X1, 4), EmitError);       // not a multiple of 8
    CHECK_THROWS_AS(e.ldr(W0, X1, 16384), EmitError);   // 4096 accesses
    CHECK_THROWS_AS(e.ldr(X0, X1, 32768), EmitError);
    CHECK_THROWS_AS(e.ldp_post(X0, X1, SP, 512), EmitError);
    CHECK_THROWS_AS(e.ldp_post(X0, X1, SP, -520), EmitError);
    CHECK_THROWS_AS(e.stp_pre(W0, W1, SP, 256), EmitError);
    CHECK_THROWS_AS(e.stp_pre(W0, W1, SP, 6), EmitError);
    CHECK_THROWS_AS(e.b_offset(134217728), EmitError);
    CHECK_THROWS_AS(e.b_offset(-134217732), EmitError);
    CHECK_THROWS_AS(e.bl_offset(2), EmitError);
    CHECK_THROWS_AS(e.b_cond_offset(Cond::EQ, 1048576), EmitError);
    CHECK_THROWS_AS(e.b_cond_offset(Cond::EQ, -1048580), EmitError);
    CHECK_THROWS_AS(e.cbz_offset(X0, 1048576), EmitError);
    CHECK_THROWS_AS(e.cbnz_offset(X0, -1048580), EmitError);
    CHECK_THROWS_AS(e.cset(W0, Cond::AL), EmitError);
    CHECK(e.code().empty());  // nothing was emitted by any failed call
}

TEST_CASE("arm64: sp and the zero register are not interchangeable") {
    Emitter e;
    CHECK_THROWS_AS(e.add(X0, SP, X1), EmitError);      // shifted-register form has no sp
    CHECK_THROWS_AS(e.orr(SP, XZR, X1), EmitError);
    CHECK_THROWS_AS(e.add_imm(X0, XZR, 1), EmitError);  // xzr in an sp slot
    CHECK_THROWS_AS(e.add_imm(XZR, X0, 1), EmitError);
    CHECK_THROWS_AS(e.subs_imm(SP, X0, 1), EmitError);  // subs writes the zero register
    CHECK_THROWS_AS(e.ldr(X0, XZR, 0), EmitError);
    CHECK_THROWS_AS(e.br(SP), EmitError);
    CHECK(e.code().empty());
}

TEST_CASE("arm64: unpredictable pair forms are rejected") {
    Emitter e;
    CHECK_THROWS_AS(e.ldp_post(X0, X0, SP, 16), EmitError);   // same destination twice
    CHECK_THROWS_AS(e.stp_pre(X1, X2, X1, -16), EmitError);   // writeback base is also data
    CHECK(e.code().empty());
}

TEST_CASE("arm64: label misuse is a hard error") {
    SUBCASE("finish with an unbound label that has a pending branch") {
        Emitter e;
        const Label l = e.make_label();
        e.b(l);
        CHECK_THROWS_AS(e.finish(), EmitError);
    }
    SUBCASE("binding twice") {
        Emitter e;
        const Label l = e.make_label();
        e.bind(l);
        CHECK_THROWS_AS(e.bind(l), EmitError);
    }
    SUBCASE("a label nobody branched to may stay unbound") {
        Emitter e;
        e.make_label();
        CHECK_NOTHROW(e.finish());
    }
    SUBCASE("a conditional branch that cannot reach its label fails when bound") {
        Emitter e;
        const Label l = e.make_label();
        e.b_cond(Cond::EQ, l);
        for (int i = 0; i < (1 << 18); ++i) {
            e.emit(0);  // pad past the +-1 MiB reach
        }
        CHECK_THROWS_AS(e.bind(l), EmitError);
    }
}
