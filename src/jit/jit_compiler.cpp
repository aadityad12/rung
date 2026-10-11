#include "jit/jit_compiler.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "runtime/object.h"
#include "runtime/value.h"

#if RUNG_NANBOX
#include "disassembler.h"
#include "jit/arm64_emitter.h"
#endif

namespace rung::jit {

// ---- the whitelist ---------------------------------------------------------------------------

namespace {

// What is wrong with a constant used by a whitelisted instruction, or null if nothing is. An int
// is always fine. nil and bools are fine to load or return (the code only stores their bits),
// but not as an operand of arithmetic or a comparison, where the JIT only has an int path.
const char* constant_problem(Value v, bool operand_of_int_op) {
    if (is_int(v)) return nullptr;
    if (is_float(v)) return "uses a float constant";
    if (is_obj(v)) return is_string(v) ? "uses a string constant" : "uses an object constant";
    return operand_of_int_op ? "has a nil or bool constant as an arithmetic operand" : nullptr;
}

// Finds the begin and end pointers of a real ObjArray's std::vector by looking for their values
// in the object's bytes. libc++ and libstdc++ both keep three pointers (begin, end, capacity),
// but nothing in the standard promises that, and a debug or hardened library could add fields.
// So the layout is checked, not assumed: each pointer must occur exactly once (capacity is made
// larger than the size so end and capacity differ), at an 8-byte aligned offset an LDR reaches.
ArrayLayout probe_array_layout() {
    static_assert(sizeof(ObjKind) == 1, "the code reads the kind with one LDRB");
    static_assert(std::is_standard_layout_v<Obj>, "offsetof(Obj, kind) must be well defined");
    std::vector<Value> elements;
    elements.reserve(4);
    elements.resize(2, make_nil());
    const ObjArray array(std::move(elements));  // never linked into a Heap
    ArrayLayout layout;
    if (array.elements.capacity() == array.elements.size()) return layout;

    const Obj* header = &array;  // the address a NaN box holds (make_obj)
    const std::ptrdiff_t vector_at = reinterpret_cast<const unsigned char*>(&array.elements) -
                                     reinterpret_cast<const unsigned char*>(header);
    constexpr std::size_t kWords = sizeof(array.elements) / sizeof(std::uintptr_t);
    std::uintptr_t words[kWords];
    std::memcpy(words, &array.elements, sizeof words);
    const auto find = [&](const Value* pointer) -> std::optional<std::ptrdiff_t> {
        std::optional<std::ptrdiff_t> found;
        for (std::size_t i = 0; i < kWords; ++i) {
            if (words[i] != reinterpret_cast<std::uintptr_t>(pointer)) continue;
            if (found) return std::nullopt;  // ambiguous
            found = vector_at + static_cast<std::ptrdiff_t>(i * sizeof(std::uintptr_t));
        }
        return found;
    };
    const std::optional<std::ptrdiff_t> begin = find(array.elements.data());
    const std::optional<std::ptrdiff_t> end = find(array.elements.data() + array.elements.size());
    // LDR's unsigned offset: a multiple of 8, at most 4095 * 8.
    const auto reachable = [](std::optional<std::ptrdiff_t> at) {
        return at && *at >= 0 && *at % 8 == 0 && *at <= 4095 * 8;
    };
    if (!reachable(begin) || !reachable(end)) return layout;
    layout.usable = true;
    layout.kind = static_cast<std::uint32_t>(offsetof(Obj, kind));
    layout.begin = static_cast<std::uint32_t>(*begin);
    layout.end = static_cast<std::uint32_t>(*end);
    return layout;
}

}  // namespace

const ArrayLayout& array_layout() {
    // Initialised once, thread-safely: the background compiler thread may be first (notes D8).
    static const ArrayLayout layout = probe_array_layout();
    return layout;
}

// A superinstruction (--superinstructions, ladder rung 3d) is only the first half of a pair: its
// word is that instruction with the opcode byte changed, and the next word is the second half,
// still an ordinary instruction (register_code.h, "Fusion"). The JIT has no dispatch to save, so
// it reads every word as the instruction it was before fusion: the first half through
// fused_first, the second half as itself. Every index keeps its meaning, so labels, jump targets
// and bail-out indices are unchanged (notes D16).
RegOp unfused_op(Instruction insn) {
    const RegOp op = insn_op(insn);
    return is_fused(op) ? fused_first(op) : op;
}

std::optional<Rejection> check_whitelist(const RegChunk& chunk) {
    if (chunk.frame_size > kMaxFrameSize) {
        return Rejection{0, "needs " + std::to_string(chunk.frame_size) +
                                " registers, more than the JIT addresses (" +
                                std::to_string(kMaxFrameSize) + ")"};
    }
    const std::size_t count = chunk.code.size();
    for (std::size_t i = 0; i < count; ++i) {
        const Instruction insn = chunk.code[i];
        // RK operand check: a register is always fine; a constant must pass constant_problem.
        auto rk_problem = [&](std::uint32_t field, std::uint8_t flag, bool int_op) -> const char* {
            if ((insn_flags(insn) & flag) == 0) return nullptr;
            return constant_problem(chunk.constants[field], int_op);
        };
        const char* problem = nullptr;
        switch (unfused_op(insn)) {
            case RegOp::Move:
            case RegOp::LoadNil:
            case RegOp::LoadTrue:
            case RegOp::LoadFalse:
            case RegOp::ReturnNil:
                break;
            case RegOp::LoadK:
                problem = constant_problem(chunk.constants[insn_bx(insn)], false);
                break;
            case RegOp::Add:
            case RegOp::Sub:
            case RegOp::Mul:
            case RegOp::Div:
            case RegOp::Mod:
            case RegOp::Eq:
            case RegOp::Ne:
            case RegOp::Lt:
            case RegOp::Le:
            case RegOp::Gt:
            case RegOp::Ge:
                problem = rk_problem(insn_b(insn), kFlagBConst, true);
                if (problem == nullptr) problem = rk_problem(insn_c(insn), kFlagCConst, true);
                break;
            case RegOp::Neg:
                problem = rk_problem(insn_b(insn), kFlagBConst, true);
                break;
            case RegOp::Return:
                problem = rk_problem(insn_b(insn), kFlagBConst, false);
                break;
            case RegOp::Jump:
            case RegOp::JumpIfFalse:
            case RegOp::JumpIfTrue: {
                // The compiler never emits a jump outside its function; checked anyway so a
                // label can never be missing in generate().
                const std::int64_t target =
                    static_cast<std::int64_t>(i) + 1 + insn_sbx(insn);
                if (target < 0 || target >= static_cast<std::int64_t>(count)) {
                    problem = "jumps outside the function";
                }
                break;
            }
            case RegOp::Not:
                problem = "uses '!', which is not in the whitelist";
                break;
            case RegOp::GetGlobal:
            case RegOp::SetGlobal:
            case RegOp::DefineGlobal:
                problem = "uses a global variable";
                break;
            case RegOp::GetUpvalue:
            case RegOp::SetUpvalue:
                problem = "uses a captured variable";
                break;
            case RegOp::Closure:
            case RegOp::Capture:
            case RegOp::Close:
                problem = "creates or closes a closure";
                break;
            case RegOp::Call:
                problem = "calls a function";
                break;
            case RegOp::Print:
                problem = "prints";
                break;
            case RegOp::IndexGet:
            case RegOp::IndexSet:
                // Any operand shape compiles: an array operand that is a constant, or an index
                // constant that is not an int, is always an error, which the code leaves to the
                // VM. Float and string constants are rejected as everywhere else.
                if (!array_layout().usable) {
                    problem = "uses an array, and this C++ library's std::vector layout is not "
                              "one the JIT reads";
                    break;
                }
                problem = rk_problem(insn_b(insn), kFlagBConst, false);
                if (problem == nullptr) problem = rk_problem(insn_c(insn), kFlagCConst, false);
                break;
            case RegOp::Array:
            case RegOp::ArrayAppend:
                // Creating an array allocates, and nothing in the whitelist may ("Arrays and
                // the garbage collector", below).
                problem = "creates an array";
                break;
            case RegOp::LtJumpIfFalse:
            case RegOp::LeJumpIfFalse:
            case RegOp::AddJump:
            case RegOp::ModAdd:
            case RegOp::DivAdd:
            case RegOp::IndexSetAdd:
                // unfused_op never returns a fused opcode; listed so a new opcode cannot be
                // missed here silently (-Wswitch).
                problem = "has a fused opcode the JIT cannot unfuse";
                break;
        }
        if (problem != nullptr) return Rejection{i, problem};
    }
    return std::nullopt;
}

#if RUNG_NANBOX

// ---- code generation -------------------------------------------------------------------------
//
// Calling convention (AAPCS64, the standard ARM64 procedure call standard, which both macOS and
// Linux follow for the registers used here). The VM calls the code as a C function
// `uint32_t f(Value* base)`:
//
//   x0        in: the frame's register 0 (`base`); out: w0 = the bytecode index to resume at.
//             The VM's register r is the 8-byte NaN-boxed value at [x0 + r*8].
//   x1..x4    constants set up on entry (below). x1..x7 are argument registers, which the
//             callee may overwrite freely.
//   x5        a fifth constant, the object tag, set up on entry only in a function that reads
//             or writes an array element (issue #26).
//   x9..x12   scratch. x9..x15 are "temporary" registers, also free for the callee.
//   x13..x15  scratch for an array access: the array's address, its element 0, its length.
//   x16, x17  not used (the linker may use them between a call and its target).
//   x18       never touched: Apple reserves it for the platform.
//   x19..x28  callee-saved; not used, so nothing needs saving.
//   x29, x30  frame pointer and return address; not touched, so `ret` returns through x30.
//   sp        not touched: the code needs no stack.
//
// The code is a leaf (it calls nothing) that uses only registers the caller already expects to
// lose, so it needs no prologue or epilogue at all: no frame record is pushed and the stack
// pointer never moves, which also keeps the 16-byte stack alignment rule trivially satisfied.
// A debugger or profiler sees the VM's frame as the caller through x30, as for any C leaf
// function.
//
// The VM's registers stay in memory (notes D3): every instruction loads its operands from the
// frame and stores its result back to the frame, so at any instruction boundary the frame holds
// exactly what the register VM would have in it. That is what makes the bail-out free.
//
// Bail-out protocol. Each instruction reads every operand and checks it (type guards, and the
// zero divisor) before it writes anything. If a check fails, the code returns the index of that
// instruction in w0, with the frame holding the state from before it. The VM sets its pc to
// that instruction and executes it itself, so a float operand takes the VM's float path, and
// `1 / 0` raises `division by zero` from the VM with the VM's message and line (notes §2.4).
// RETURN and RETURN_NIL also just return their own index: the VM executes the return itself
// (closing upvalues, popping the frame), so the JIT never needs to know how frames work.
//
// Arrays and the garbage collector (issue #26). An array access holds raw pointers in machine
// registers: the array object's address and the address of its element 0. The invariant that
// makes that safe: **no raw heap pointer stays in a machine register past the end of the
// bytecode instruction that loaded it, and nothing in the whitelist can allocate.** Each
// INDEX_GET / INDEX_SET loads the array from its frame register, then its element pointers, and
// uses them at once; the next instruction starts again from the frame. Machine code calls
// nothing, so between its entry and its `ret` nothing else runs on the engine's thread: no
// allocation, so no collection (the collector only runs inside an allocation, notes D10), and no
// VM code, so the element storage cannot be reallocated (only ARRAY_APPEND resizes it, and it is
// not in the whitelist). The array stays alive because a frame register refers to it and the
// collector marks every frame register (RegisterEngine::mark_roots), and it never moves: the
// collector is mark-sweep, which frees objects but never relocates one. A store needs no write
// barrier for the same reason: the collector is stop-the-world and not generational, so a
// pointer written into an array between collections is simply found by the next mark. A moving,
// incremental or generational collector would break these assumptions, and this code would
// have to change with it. The background compiler thread (notes D8) only generates code; the
// code runs on the engine's thread, so the arrays it touches are never shared between threads.

namespace {

using namespace rung::arm64;

static_assert(sizeof(Value) == 8, "the JIT addresses NaN-boxed 8-byte registers (notes D15)");

constexpr XReg kBase = X0;                    // the frame's register 0
constexpr XReg kIntTag = X1;                  // nanbox::kIntTag: OR it in to box an int
constexpr WReg kIntTagHigh = W2;              // kIntTag >> 32: the high word an int must have
constexpr XReg kFalseBits = X3;               // nanbox::kFalse; OR in 1 for true
constexpr XReg kNilBits = X4;                 // nanbox::kNil
constexpr XReg kObjTag = X5;                  // nanbox::kObjTag; set up only if arrays are used
constexpr XReg kB = X9;                       // operand B (an int in the low 32 bits)
constexpr XReg kC = X10;                      // operand C
constexpr XReg kTmp = X11;                    // scratch for guards and `%`
constexpr XReg kResult = X12;                 // the result before it is boxed
constexpr XReg kIndex = X10;                  // an array index, sign-extended (operand B or C)
constexpr XReg kArray = X13;                  // the array's address (its Obj*)
constexpr XReg kElements = X14;               // the address of its element 0
constexpr XReg kLength = X15;                 // its length, in elements

constexpr WReg w(XReg x) { return static_cast<WReg>(static_cast<std::uint8_t>(x)); }

constexpr std::uint32_t slot_offset(std::uint32_t reg) {
    return reg * static_cast<std::uint32_t>(sizeof(Value));
}

class CodeGen {
public:
    explicit CodeGen(const RegChunk& chunk) : chunk_(chunk) {
        const std::size_t count = chunk.code.size();
        labels_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) labels_.push_back(e_.make_label());
        exits_.resize(count);
    }

    GeneratedCode run() {
        GeneratedCode out;
        // Entry: the four constants the instructions share. Each is one MOVZ.
        e_.mov_imm(kIntTag, nanbox::kIntTag);
        e_.mov_imm(kIntTagHigh, static_cast<std::uint32_t>(nanbox::kIntTag >> 32));
        e_.mov_imm(kFalseBits, nanbox::kFalse);
        e_.mov_imm(kNilBits, nanbox::kNil);
        if (uses_arrays()) e_.mov_imm(kObjTag, nanbox::kObjTag);  // one MOVZ as well
        for (std::size_t i = 0; i < chunk_.code.size(); ++i) {
            e_.bind(labels_[i]);
            out.starts.push_back(e_.code().size());
            instruction(i);
        }
        // The exits that guards branch to, after the body so the hot path falls through.
        for (std::size_t i = 0; i < exits_.size(); ++i) {
            if (!exits_[i]) continue;
            e_.bind(*exits_[i]);
            exit_with(i);
        }
        e_.finish();
        out.words = e_.code();
        return out;
    }

private:
    bool uses_arrays() const {
        for (Instruction insn : chunk_.code) {
            const RegOp op = unfused_op(insn);
            if (op == RegOp::IndexGet || op == RegOp::IndexSet) return true;
        }
        return false;
    }

    // `mov w0, #index; ret`: back to the VM, which resumes at bytecode instruction `index`.
    void exit_with(std::size_t index) {
        e_.mov_imm(W0, static_cast<std::uint32_t>(index));
        e_.ret();
    }

    // The shared exit for a failed check in instruction `index`, made on first use.
    Label exit_label(std::size_t index) {
        if (!exits_[index]) exits_[index] = e_.make_label();
        return *exits_[index];
    }

    // Puts an int operand in the low 32 bits of `dst`. A constant is materialised (the whitelist
    // made sure it is an int). A register is loaded and guarded: its high 32 bits must be the
    // int tag's, else the code bails out to the VM at `index`. Nothing needs unboxing: the int
    // already is the low word, and 32-bit instructions read only that.
    void int_operand(std::size_t index, std::uint32_t field, bool is_constant, XReg dst) {
        if (is_constant) {
            e_.mov_imm(w(dst), static_cast<std::uint32_t>(as_int(chunk_.constants[field])));
            return;
        }
        e_.ldr(dst, kBase, slot_offset(field));
        e_.lsr(kTmp, dst, 32);                         // the tag word
        e_.cmp(w(kTmp), kIntTagHigh);                  // is it an int's?
        e_.b_cond(Cond::NE, exit_label(index));       // type guard failed: bail out
    }

    void operands_bc(std::size_t index, Instruction insn) {
        int_operand(index, insn_b(insn), (insn_flags(insn) & kFlagBConst) != 0, kB);
        int_operand(index, insn_c(insn), (insn_flags(insn) & kFlagCConst) != 0, kC);
    }

    // `/` and `%`: a zero divisor leaves the machine code for the VM, which raises the error.
    // A constant divisor is known now: no check if it is not zero, an unconditional exit if it
    // is (the code after that exit is never reached).
    void check_divisor(std::size_t index, Instruction insn) {
        if ((insn_flags(insn) & kFlagCConst) == 0) {
            e_.cbz(w(kC), exit_label(index));
        } else if (as_int(chunk_.constants[insn_c(insn)]) == 0) {
            e_.b(exit_label(index));
        }
    }

    // A 32-bit instruction zeroes the high half of its X register, so OR-ing the tag in boxes
    // the int; then it goes to register A.
    void store_int_result(Instruction insn) {
        e_.orr(kResult, kResult, kIntTag);
        e_.str(kResult, kBase, slot_offset(insn_a(insn)));
    }

    void compare(std::size_t index, Instruction insn, Cond cond) {
        operands_bc(index, insn);
        e_.cmp(w(kB), w(kC));          // signed 32-bit compare
        e_.cset(w(kResult), cond);     // 0 or 1
        e_.orr(kResult, kResult, kFalseBits);  // false | 1 is true (notes D15)
        e_.str(kResult, kBase, slot_offset(insn_a(insn)));
    }

    // Branches to `target` if the value in `value` is falsy: nil or false are the only falsy
    // values (notes §2.2), so this is two compares and never needs a guard.
    void branch_if_falsy(XReg value, Label target) {
        e_.cmp(value, kFalseBits);
        e_.b_cond(Cond::EQ, target);
        e_.cmp(value, kNilBits);
        e_.b_cond(Cond::EQ, target);
    }

    // Puts the array in register `field` into kArray, or bails out at `index` if it is not an
    // array. An object's NaN box is kObjTag | address (notes D15), so XOR-ing the tag out leaves
    // the address with a zero top 16 bits, and anything that is not an object leaves some of
    // them set. Then the header's kind byte must say Array: a string or a function is an object
    // too. Returns false if the operand is a constant: no constant is an array, so the
    // instruction is an unconditional exit and the VM raises `can only index arrays`.
    bool array_operand(std::size_t index, std::uint32_t field, bool is_constant) {
        if (is_constant) {
            e_.b(exit_label(index));
            return false;
        }
        e_.ldr(kArray, kBase, slot_offset(field));
        e_.eor(kArray, kArray, kObjTag);
        e_.lsr(kTmp, kArray, 48);
        e_.cbnz(kTmp, exit_label(index));               // not an object
        e_.ldrb(w(kTmp), kArray, array_layout().kind);
        e_.cmp_imm(w(kTmp), static_cast<std::uint32_t>(ObjKind::Array));
        e_.b_cond(Cond::NE, exit_label(index));       // an object, but not an array
        return true;
    }

    // Puts the index from RK `field` into kIndex, the array's element 0 into kElements and its
    // length into kLength, and bails out at `index` unless the index is an int with
    // 0 <= index < length. Returns false if that is known to fail already (a constant that is
    // not an int, or a negative one): the instruction is then an unconditional exit.
    bool element_index(std::size_t index, std::uint32_t field, bool is_constant) {
        if (is_constant) {
            const Value k = chunk_.constants[field];
            if (!is_int(k) || as_int(k) < 0) {
                e_.b(exit_label(index));
                return false;
            }
            // A W write zeroes the high half, so kIndex holds the non-negative index as 64 bits.
            e_.mov_imm(w(kIndex), static_cast<std::uint32_t>(as_int(k)));
        } else {
            int_operand(index, field, false, kIndex);  // type guard: the index is an int
            e_.sxtw(kIndex, w(kIndex));                // so -1 becomes 0xFFFF'FFFF'FFFF'FFFF
        }
        // The length comes from the vector's two pointers, reloaded for every access.
        e_.ldr(kElements, kArray, array_layout().begin);
        e_.ldr(kLength, kArray, array_layout().end);
        e_.sub(kLength, kLength, kElements);
        e_.lsr(kLength, kLength, 3);                   // bytes to 8-byte elements
        // One unsigned compare checks both ends: a negative index, sign-extended, is larger than
        // any length when read as unsigned, so `index >= length` (B.HS) catches it too.
        e_.cmp(kIndex, kLength);
        e_.b_cond(Cond::HS, exit_label(index));
        return true;
    }

    Label jump_target(std::size_t index, Instruction insn) {
        const std::int64_t target = static_cast<std::int64_t>(index) + 1 + insn_sbx(insn);
        return labels_[static_cast<std::size_t>(target)];
    }

    void instruction(std::size_t i) {
        const Instruction insn = chunk_.code[i];
        switch (unfused_op(insn)) {
            case RegOp::Move:
                // Any value: it is only copied, so no guard.
                e_.ldr(kB, kBase, slot_offset(insn_b(insn)));
                e_.str(kB, kBase, slot_offset(insn_a(insn)));
                return;
            case RegOp::LoadK:
                e_.mov_imm(kB, value_bits(chunk_.constants[insn_bx(insn)]));
                e_.str(kB, kBase, slot_offset(insn_a(insn)));
                return;
            case RegOp::LoadNil:
                e_.str(kNilBits, kBase, slot_offset(insn_a(insn)));
                return;
            case RegOp::LoadFalse:
                e_.str(kFalseBits, kBase, slot_offset(insn_a(insn)));
                return;
            case RegOp::LoadTrue:
                e_.add_imm(kB, kFalseBits, 1);
                e_.str(kB, kBase, slot_offset(insn_a(insn)));
                return;

            // W registers wrap at 32 bits, exactly like Rung ints (notes D1).
            case RegOp::Add:
                operands_bc(i, insn);
                e_.add(w(kResult), w(kB), w(kC));
                store_int_result(insn);
                return;
            case RegOp::Sub:
                operands_bc(i, insn);
                e_.sub(w(kResult), w(kB), w(kC));
                store_int_result(insn);
                return;
            case RegOp::Mul:
                operands_bc(i, insn);
                e_.mul(w(kResult), w(kB), w(kC));
                store_int_result(insn);
                return;
            case RegOp::Div:
                // SDIV by zero silently gives 0 on ARM64, so the zero divisor is checked
                // explicitly and left to the VM, which raises the error (notes §2.1).
                // INT32_MIN / -1 needs nothing: SDIV gives INT32_MIN, the rule's answer.
                operands_bc(i, insn);
                check_divisor(i, insn);
                e_.sdiv(w(kResult), w(kB), w(kC));
                store_int_result(insn);
                return;
            case RegOp::Mod:
                // No remainder instruction: b - (b / c) * c, with SDIV then MSUB (notes §2.1).
                // The sign follows b, as in C. INT32_MIN % -1: the quotient wraps to INT32_MIN,
                // INT32_MIN * -1 wraps to INT32_MIN, and the difference is 0.
                operands_bc(i, insn);
                check_divisor(i, insn);
                e_.sdiv(w(kTmp), w(kB), w(kC));
                e_.msub(w(kResult), w(kTmp), w(kC), w(kB));
                store_int_result(insn);
                return;
            case RegOp::Neg:
                int_operand(i, insn_b(insn), (insn_flags(insn) & kFlagBConst) != 0, kB);
                e_.neg(w(kResult), w(kB));  // -INT32_MIN wraps to INT32_MIN
                store_int_result(insn);
                return;

            // Int-only fast paths. Two ints compare as ints; anything else (a float, a string
            // compared with ==) fails the guard and the VM does it.
            case RegOp::Eq: compare(i, insn, Cond::EQ); return;
            case RegOp::Ne: compare(i, insn, Cond::NE); return;
            case RegOp::Lt: compare(i, insn, Cond::LT); return;
            case RegOp::Le: compare(i, insn, Cond::LE); return;
            case RegOp::Gt: compare(i, insn, Cond::GT); return;
            case RegOp::Ge: compare(i, insn, Cond::GE); return;

            case RegOp::Jump:
                e_.b(jump_target(i, insn));
                return;
            case RegOp::JumpIfFalse:
                e_.ldr(kB, kBase, slot_offset(insn_a(insn)));
                branch_if_falsy(kB, jump_target(i, insn));
                return;
            case RegOp::JumpIfTrue: {
                const Label fall_through = e_.make_label();
                e_.ldr(kB, kBase, slot_offset(insn_a(insn)));
                branch_if_falsy(kB, fall_through);
                e_.b(jump_target(i, insn));
                e_.bind(fall_through);
                return;
            }

            // R[A] = RK(B)[RK(C)]. Every check comes before the one write, to register A, so a
            // failed one leaves the frame as it was and the VM raises the exact error (§2.5).
            case RegOp::IndexGet:
                if (!array_operand(i, insn_b(insn), (insn_flags(insn) & kFlagBConst) != 0)) {
                    return;
                }
                if (!element_index(i, insn_c(insn), (insn_flags(insn) & kFlagCConst) != 0)) {
                    return;
                }
                e_.ldr_scaled(kResult, kElements, kIndex);  // any value: no guard
                e_.str(kResult, kBase, slot_offset(insn_a(insn)));
                return;
            // R[A][RK(B)] = RK(C). The one write is to the element itself, after every check.
            case RegOp::IndexSet:
                if (!array_operand(i, insn_a(insn), false)) return;
                if (!element_index(i, insn_b(insn), (insn_flags(insn) & kFlagBConst) != 0)) {
                    return;
                }
                if ((insn_flags(insn) & kFlagCConst) != 0) {
                    e_.mov_imm(kResult, value_bits(chunk_.constants[insn_c(insn)]));
                } else {
                    e_.ldr(kResult, kBase, slot_offset(insn_c(insn)));
                }
                e_.str_scaled(kResult, kElements, kIndex);
                return;

            case RegOp::Return:
            case RegOp::ReturnNil:
                exit_with(i);
                return;

            default:
                // check_whitelist rejects every other opcode before generate() is called; if
                // that ever breaks, refuse loudly instead of emitting nothing for it.
                throw EmitError(std::string("jit: no code generation for ") +
                                reg_opcode_name(unfused_op(insn)));
        }
    }

    const RegChunk& chunk_;
    Emitter e_;
    std::vector<Label> labels_;               // one per bytecode instruction
    std::vector<std::optional<Label>> exits_;  // bail-out exits, per bytecode instruction
};

}  // namespace

GeneratedCode generate(const RegChunk& chunk) { return CodeGen(chunk).run(); }

#endif  // RUNG_NANBOX

}  // namespace rung::jit
