#include "jit/arm64_emitter.h"

#include <string>

namespace rung::arm64 {

namespace {

constexpr unsigned kZr = 31;
constexpr unsigned kSp = 32;

[[noreturn]] void fail(const std::string& message) {
    throw EmitError("arm64 emitter: " + message);
}

// Register field for a slot where 31 means the zero register.
uint32_t zr_slot(unsigned r) {
    if (r == kSp) {
        fail("sp is not allowed here (register 31 means the zero register in this slot)");
    }
    return r;
}

// Register field for a slot where 31 means the stack pointer.
uint32_t sp_slot(unsigned r) {
    if (r == kZr) {
        fail("the zero register is not allowed here (register 31 means sp in this slot)");
    }
    return r == kSp ? 31u : r;
}

void check_range(int64_t value, int64_t lo, int64_t hi, const char* what) {
    if (value < lo || value > hi) {
        fail(std::string(what) + " " + std::to_string(value) + " is out of range [" +
             std::to_string(lo) + ", " + std::to_string(hi) + "]");
    }
}

// Bit 31 of most instructions: 1 selects the 64-bit (X) form.
uint32_t sf_bit(bool sf) { return sf ? 0x80000000u : 0u; }

// A branch offset in bytes -> the signed word offset masked to `bits` bits, range checked.
uint32_t branch_field(int64_t byte_offset, unsigned bits) {
    if (byte_offset % 4 != 0) {
        fail("branch offset " + std::to_string(byte_offset) + " is not a multiple of 4");
    }
    const int64_t words = byte_offset / 4;
    const int64_t limit = int64_t{1} << (bits - 1);
    check_range(words, -limit, limit - 1, "branch offset (in instructions)");
    return static_cast<uint32_t>(words) & ((uint32_t{1} << bits) - 1);
}

constexpr uint32_t kBase = 0x14000000;  // B; BL is the same with bit 31 set
constexpr uint32_t kBCond = 0x54000000;
constexpr uint32_t kCbz = 0x34000000;  // 32-bit CBZ; bit 24 = NZ, bit 31 = X

// The word for one 16-bit chunk sequence, shared by the W and X versions of mov_imm.
template <class Emit>
void mov_imm_impl(unsigned chunks, uint64_t value, Emit&& emit) {
    unsigned zeros = 0;
    unsigned ones = 0;
    for (unsigned i = 0; i < chunks; ++i) {
        const uint32_t c = static_cast<uint32_t>(value >> (16 * i)) & 0xFFFFu;
        zeros += c == 0x0000u;
        ones += c == 0xFFFFu;
    }
    // Start from all-zero (MOVZ) or all-one (MOVN) and only touch chunks that differ. Ties go
    // to MOVZ.
    const bool use_movn = ones > zeros;
    const uint32_t skip = use_movn ? 0xFFFFu : 0x0000u;
    bool first = true;
    for (unsigned i = 0; i < chunks; ++i) {
        const uint32_t c = static_cast<uint32_t>(value >> (16 * i)) & 0xFFFFu;
        if (c == skip) {
            continue;
        }
        if (first) {
            emit(use_movn ? Wide::Movn : Wide::Movz, use_movn ? (~c & 0xFFFFu) : c, 16 * i);
            first = false;
        } else {
            emit(Wide::Movk, c, 16 * i);
        }
    }
    if (first) {  // every chunk equals the skip value: 0 or all ones
        emit(use_movn ? Wide::Movn : Wide::Movz, 0u, 0u);
    }
}

}  // namespace

// ---- arithmetic -----------------------------------------------------------------------------

void Emitter::addsub_reg_(AddSub op, bool sf, unsigned d, unsigned n, unsigned m) {
    // sf op S 01011 shift(2)=0 0 Rm imm6=0 Rn Rd
    static constexpr uint32_t kOp[] = {0x0B000000, 0x4B000000, 0x2B000000, 0x6B000000};
    emit(sf_bit(sf) | kOp[static_cast<unsigned>(op)] | zr_slot(m) << 16 | zr_slot(n) << 5 |
         zr_slot(d));
}

void Emitter::addsub_imm_(AddSub op, bool sf, unsigned d, unsigned n, uint32_t imm12,
                          bool shift12) {
    // sf op S 100010 sh imm12 Rn Rd
    static constexpr uint32_t kOp[] = {0x11000000, 0x51000000, 0x31000000, 0x71000000};
    check_range(imm12, 0, 4095, "add/sub immediate");
    // ADDS/SUBS write the zero register in slot 31 (that is what CMP is); ADD/SUB write SP.
    const bool sets_flags = op == AddSub::Adds || op == AddSub::Subs;
    const uint32_t rd = sets_flags ? zr_slot(d) : sp_slot(d);
    emit(sf_bit(sf) | kOp[static_cast<unsigned>(op)] | (shift12 ? 1u << 22 : 0u) | imm12 << 10 |
         sp_slot(n) << 5 | rd);
}

void Emitter::mul_add_(bool subtract, bool sf, unsigned d, unsigned n, unsigned m, unsigned a) {
    // sf 00 11011 000 Rm o0 Ra Rn Rd     o0 = 0 MADD, 1 MSUB
    emit(sf_bit(sf) | 0x1B000000u | zr_slot(m) << 16 | (subtract ? 1u << 15 : 0u) |
         zr_slot(a) << 10 | zr_slot(n) << 5 | zr_slot(d));
}

void Emitter::sdiv_(bool sf, unsigned d, unsigned n, unsigned m) {
    // sf 0 0 11010110 Rm 00001 1 Rn Rd
    emit(sf_bit(sf) | 0x1AC00C00u | zr_slot(m) << 16 | zr_slot(n) << 5 | zr_slot(d));
}

// ---- logic and shifts -----------------------------------------------------------------------

void Emitter::logic_reg_(Logic op, bool sf, unsigned d, unsigned n, unsigned m) {
    // sf opc(2) 01010 shift(2)=0 N=0 Rm imm6=0 Rn Rd
    static constexpr uint32_t kOp[] = {0x0A000000, 0x2A000000, 0x4A000000, 0x6A000000};
    emit(sf_bit(sf) | kOp[static_cast<unsigned>(op)] | zr_slot(m) << 16 | zr_slot(n) << 5 |
         zr_slot(d));
}

void Emitter::shift_imm_(Shift op, bool sf, unsigned d, unsigned n, unsigned amount) {
    // UBFM/SBFM: sf opc 100110 N immr imms Rn Rd. N must equal sf.
    const unsigned width = sf ? 64 : 32;
    check_range(amount, 0, width - 1, "shift amount");
    const uint32_t base = (op == Shift::Asr ? 0x13000000u : 0x53000000u) | (sf ? 0x80400000u : 0u);
    uint32_t immr;
    uint32_t imms;
    if (op == Shift::Lsl) {
        // lsl #s == ubfm immr = -s mod width, imms = width-1-s
        immr = (width - amount) % width;
        imms = width - 1 - amount;
    } else {
        // lsr/asr #s == ubfm/sbfm immr = s, imms = width-1
        immr = amount;
        imms = width - 1;
    }
    emit(base | immr << 16 | imms << 10 | zr_slot(n) << 5 | zr_slot(d));
}

// ---- moves ----------------------------------------------------------------------------------

void Emitter::wide_(Wide op, bool sf, unsigned d, uint32_t imm16, unsigned shift) {
    // sf opc(2) 100101 hw imm16 Rd     opc: MOVN 00, MOVZ 10, MOVK 11
    static constexpr uint32_t kOp[] = {0x52800000, 0x12800000, 0x72800000};  // Movz, Movn, Movk
    check_range(imm16, 0, 0xFFFF, "move-wide immediate");
    if (shift % 16 != 0 || shift >= (sf ? 64u : 32u)) {
        fail("move-wide shift " + std::to_string(shift) + " must be 0, 16" +
             (sf ? ", 32 or 48" : "") + " for this register width");
    }
    emit(sf_bit(sf) | kOp[static_cast<unsigned>(op)] | (shift / 16) << 21 | imm16 << 5 |
         zr_slot(d));
}

void Emitter::mov(WReg d, WReg n) {
    if (d == WSP || n == WSP) {
        addsub_imm_(AddSub::Add, false, d, n, 0, false);
    } else {
        logic_reg_(Logic::Orr, false, d, WZR, n);
    }
}

void Emitter::mov(XReg d, XReg n) {
    if (d == SP || n == SP) {
        addsub_imm_(AddSub::Add, true, d, n, 0, false);
    } else {
        logic_reg_(Logic::Orr, true, d, XZR, n);
    }
}

void Emitter::mov_imm(WReg d, uint32_t value) {
    mov_imm_impl(2, value, [&](Wide op, uint32_t imm, unsigned shift) {
        wide_(op, false, d, imm, shift);
    });
}

void Emitter::mov_imm(XReg d, uint64_t value) {
    mov_imm_impl(4, value, [&](Wide op, uint32_t imm, unsigned shift) {
        wide_(op, true, d, imm, shift);
    });
}

// ---- memory ---------------------------------------------------------------------------------

void Emitter::ldr_str_(bool load, bool sf, unsigned rt, unsigned base, uint32_t byte_offset) {
    // 1x 111 0 01 opc imm12 Rn Rt     opc: 00 STR, 01 LDR; imm12 is scaled by the access size
    const uint32_t size = sf ? 8 : 4;
    if (byte_offset % size != 0) {
        fail("load/store offset " + std::to_string(byte_offset) + " is not a multiple of " +
             std::to_string(size));
    }
    check_range(byte_offset / size, 0, 4095, "load/store offset (in accesses)");
    emit((sf ? 0xF9000000u : 0xB9000000u) | (load ? 1u << 22 : 0u) | byte_offset / size << 10 |
         sp_slot(base) << 5 | zr_slot(rt));
}

void Emitter::pair_(bool load, Writeback wb, bool sf, unsigned t1, unsigned t2, unsigned base,
                    int32_t byte_offset) {
    // opc(2) 101 0 001/011 L imm7 Rt2 Rn Rt     001 = post-index, 011 = pre-index
    const int32_t size = sf ? 8 : 4;
    if (byte_offset % size != 0) {
        fail("pair offset " + std::to_string(byte_offset) + " is not a multiple of " +
             std::to_string(size));
    }
    check_range(byte_offset / size, -64, 63, "pair offset (in registers)");
    const uint32_t rn = sp_slot(base);
    // Writing the base back while also loading/storing it is unpredictable on hardware.
    if (base != SP && (base == t1 || base == t2)) {
        fail("pair with writeback must not use the base register as a data register");
    }
    if (load && t1 == t2) {
        fail("ldp with two identical destination registers is unpredictable");
    }
    const uint32_t imm7 = static_cast<uint32_t>(byte_offset / size) & 0x7Fu;
    emit((sf ? 0xA8800000u : 0x28800000u) | (wb == Writeback::Pre ? 1u << 24 : 0u) |
         (load ? 1u << 22 : 0u) | imm7 << 15 | zr_slot(t2) << 10 | rn << 5 | zr_slot(t1));
}

// ---- control flow ---------------------------------------------------------------------------

void Emitter::b_offset(int64_t byte_offset) { emit(kBase | branch_field(byte_offset, 26)); }

void Emitter::bl_offset(int64_t byte_offset) {
    emit(kBase | 0x80000000u | branch_field(byte_offset, 26));
}

void Emitter::b_cond_offset(Cond c, int64_t byte_offset) {
    emit(kBCond | branch_field(byte_offset, 19) << 5 | static_cast<uint32_t>(c));
}

void Emitter::cbz_(bool nonzero, bool sf, unsigned rt, int64_t byte_offset) {
    emit(sf_bit(sf) | kCbz | (nonzero ? 1u << 24 : 0u) | branch_field(byte_offset, 19) << 5 |
         zr_slot(rt));
}

void Emitter::br(XReg n) { emit(0xD61F0000u | zr_slot(n) << 5); }
void Emitter::blr(XReg n) { emit(0xD63F0000u | zr_slot(n) << 5); }
void Emitter::ret(XReg n) { emit(0xD65F0000u | zr_slot(n) << 5); }

void Emitter::cset_(bool sf, unsigned d, Cond c) {
    if (c == Cond::AL) {
        fail("cset cannot use the AL condition");
    }
    // CSET d, c is CSINC d, zr, zr, invert(c). sf 0 0 11010100 Rm cond 0 1 Rn Rd. Flipping the
    // low bit of a condition code inverts it (EQ<->NE, HS<->LO, ...).
    const uint32_t inverted = static_cast<uint32_t>(c) ^ 1u;
    emit(sf_bit(sf) | 0x1A800400u | kZr << 16 | inverted << 12 | kZr << 5 | zr_slot(d));
}

// ---- labels ---------------------------------------------------------------------------------

Label Emitter::make_label() {
    labels_.emplace_back();
    return Label{static_cast<uint32_t>(labels_.size() - 1)};
}

void Emitter::branch_to_label_(uint32_t word, Fixup kind, Label l) {
    if (l.id >= labels_.size()) {
        fail("unknown label");
    }
    LabelState& state = labels_[l.id];
    emit(word);  // offset field is zero for now
    const size_t index = code_.size() - 1;
    if (state.bound) {
        patch_(index, kind, state.index);
    } else {
        state.pending.push_back({index, kind});
    }
}

void Emitter::patch_(size_t index, Fixup kind, size_t target_index) {
    const int64_t byte_offset =
        (static_cast<int64_t>(target_index) - static_cast<int64_t>(index)) * 4;
    if (kind == Fixup::Imm26) {
        code_[index] |= branch_field(byte_offset, 26);
    } else {
        code_[index] |= branch_field(byte_offset, 19) << 5;
    }
}

void Emitter::bind(Label l) {
    if (l.id >= labels_.size()) {
        fail("unknown label");
    }
    LabelState& state = labels_[l.id];
    if (state.bound) {
        fail("label bound twice");
    }
    state.bound = true;
    state.index = code_.size();
    for (const Pending& p : state.pending) {
        patch_(p.index, p.kind, state.index);
    }
    state.pending.clear();
}

void Emitter::finish() const {
    for (const LabelState& state : labels_) {
        if (!state.pending.empty()) {
            fail("a branch refers to a label that was never bound");
        }
    }
}

void Emitter::b(Label l) { branch_to_label_(kBase, Fixup::Imm26, l); }
void Emitter::bl(Label l) { branch_to_label_(kBase | 0x80000000u, Fixup::Imm26, l); }
void Emitter::b_cond(Cond c, Label l) {
    branch_to_label_(kBCond | static_cast<uint32_t>(c), Fixup::Imm19, l);
}
void Emitter::cbz_label_(bool nonzero, bool sf, unsigned rt, Label l) {
    branch_to_label_(sf_bit(sf) | kCbz | (nonzero ? 1u << 24 : 0u) | zr_slot(rt), Fixup::Imm19, l);
}

}  // namespace rung::arm64
