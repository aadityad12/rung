#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime/value.h"

namespace rung {

// Register-VM bytecode (notes D14). Where the stack VM's instructions push and pop an implicit
// stack, these name their operand slots ("registers") directly, so `a = b + c * d` is two
// instructions instead of seven. The design follows "The Implementation of Lua 5.0" (Ierusalimschy,
// de Figueiredo, Celes): locals live in fixed registers of the frame, temporaries are allocated
// above them in stack order, a call puts the callee and its arguments in consecutive registers
// and the result comes back in the callee's register. It differs in the instruction word (below)
// and in having no compare-and-jump instructions yet (superinstructions are ladder rung 3d).
//
// One instruction is one fixed 64-bit word:
//
//     bit 63        56 55        40 39        24 23         8 7        0
//         +-----------+------------+------------+------------+----------+
//         |   flags   |     C      |     B      |     A      |    op    |
//         +-----------+------------+------------+------------+----------+
//              8           16           16           16           8
//
// `A`, `B` and `C` are 16-bit unsigned operands. "Bx" is `B` and `C` read as one 32-bit
// unsigned number (B is the low half); "sBx" is the same bits as a signed 32-bit number, used by
// jumps: the target of a jump at instruction index i with offset d is i + 1 + d.
//
// RK operands. Where an instruction below says RK(B) or RK(C), the operand is a register, or, if
// the matching flag bit is set, an index into the function's constant pool. The flag lives in
// the spare byte, so a constant index gets all 16 bits of its field. A constant whose pool index
// does not fit 16 bits is first loaded into a temporary with LOADK (whose 32-bit Bx reaches any
// constant), so the compiler never rejects a function the stack compiler accepts (notes D11).
constexpr std::uint8_t kFlagBConst = 1;  // B is a constant index, not a register
constexpr std::uint8_t kFlagCConst = 2;  // C is a constant index, not a register

constexpr std::uint32_t kMaxRegister = 0xFFFFu;    // registers are 16 bits
constexpr std::uint32_t kMaxRkConstant = 0xFFFFu;  // largest constant index an RK operand holds

// Registers of one call frame are numbered from 0 at the frame's base. A call puts the callee in
// register A and its arguments in A+1.., and the callee's frame base is A+1: its parameters are
// the registers 0..arity-1 and there is no "slot 0 = the function" as in the stack VM. The
// result is written back to register A of the caller. Comments below say what R[x] (a register),
// K[x] (a constant) and RK(x) (either) are.
enum class RegOp : std::uint8_t {
    // Loads and moves.
    Move,          // A B        R[A] = R[B]
    LoadK,         // A Bx       R[A] = K[Bx]
    LoadNil,       // A          R[A] = nil
    LoadTrue,      // A          R[A] = true
    LoadFalse,     // A          R[A] = false
    // Variables. Globals are looked up by name at run time: Bx is the constant index of the
    // interned name (deliberately slow; inline caching, rung 3e, removes it).
    GetGlobal,     // A Bx       R[A] = globals[K[Bx]]; error if undefined
    SetGlobal,     // A Bx       globals[K[Bx]] = R[A]; error if undefined
    DefineGlobal,  // A Bx       globals[K[Bx]] = R[A] (creates or overwrites)
    GetUpvalue,    // A B        R[A] = *closure.upvalues[B]
    SetUpvalue,    // A B        *closure.upvalues[B] = R[A]
    // Arithmetic and comparison: the result is a value in R[A]. Operands are RK.
    Add, Sub, Mul, Div, Mod,   // A RK(B) RK(C)   R[A] = RK(B) op RK(C)
    Eq, Ne, Lt, Le, Gt, Ge,    // A RK(B) RK(C)   R[A] = RK(B) op RK(C)  (a bool)
    Neg,           // A RK(B)    R[A] = -RK(B)
    Not,           // A RK(B)    R[A] = !RK(B)
    // Control flow. There is one Jump for both directions: a loop is a Jump with a negative
    // offset. JumpIfFalse / JumpIfTrue do not change R[A] (`and` / `or` keep their value).
    Jump,          // sBx        pc += sBx
    JumpIfFalse,   // A sBx      if R[A] is falsy, pc += sBx
    JumpIfTrue,    // A sBx      if R[A] is truthy, pc += sBx
    // Functions.
    Call,          // A B        call R[A] with the B arguments R[A+1..A+B]; the result goes to R[A]
    Closure,       // A Bx       R[A] = a closure of the function K[Bx], followed by one Capture
                   //            word for each upvalue of that function (they are operands of
                   //            this instruction: the VM reads them and skips them)
    Capture,       // A B        operand of Closure: A = 1 captures the enclosing frame's register
                   //            B, A = 0 copies the enclosing closure's upvalue B
    Close,         // A          close every open upvalue that points at a register >= A
    Return,        // RK(B)      return RK(B); closes the frame's upvalues
    ReturnNil,     //            return nil (falling off the end of a function)
    // Statements and arrays.
    Print,         // RK(B)      print RK(B)
    Array,         // A B C      R[A] = a new array holding R[B..B+C-1] (C <= kArrayBatch)
    ArrayAppend,   // A B C      append R[B..B+C-1] to the array R[A] (array literals with more
                   //            than kArrayBatch elements are built a batch at a time)
    IndexGet,      // A RK(B) RK(C)   R[A] = RK(B)[RK(C)]
    IndexSet,      // A RK(B) RK(C)   R[A][RK(B)] = RK(C)   (A is a register; no value is produced)
};

// The most elements one Array / ArrayAppend instruction takes; they are evaluated into
// consecutive registers first, so this bounds the registers an array literal needs.
constexpr int kArrayBatch = 50;

using Instruction = std::uint64_t;

constexpr Instruction make_abc(RegOp op, std::uint32_t a, std::uint32_t b, std::uint32_t c,
                               std::uint8_t flags = 0) {
    return static_cast<Instruction>(op) | (static_cast<Instruction>(a & 0xFFFFu) << 8) |
           (static_cast<Instruction>(b & 0xFFFFu) << 24) |
           (static_cast<Instruction>(c & 0xFFFFu) << 40) | (static_cast<Instruction>(flags) << 56);
}
// Bx occupies the B and C fields: B is its low 16 bits.
constexpr Instruction make_abx(RegOp op, std::uint32_t a, std::uint32_t bx) {
    return static_cast<Instruction>(op) | (static_cast<Instruction>(a & 0xFFFFu) << 8) |
           (static_cast<Instruction>(bx) << 24);
}
constexpr Instruction make_asbx(RegOp op, std::uint32_t a, std::int32_t sbx) {
    return make_abx(op, a, static_cast<std::uint32_t>(sbx));
}

constexpr RegOp insn_op(Instruction i) { return static_cast<RegOp>(i & 0xFFu); }
constexpr std::uint32_t insn_a(Instruction i) {
    return static_cast<std::uint32_t>(i >> 8) & 0xFFFFu;
}
constexpr std::uint32_t insn_b(Instruction i) {
    return static_cast<std::uint32_t>(i >> 24) & 0xFFFFu;
}
constexpr std::uint32_t insn_c(Instruction i) {
    return static_cast<std::uint32_t>(i >> 40) & 0xFFFFu;
}
constexpr std::uint32_t insn_bx(Instruction i) { return static_cast<std::uint32_t>(i >> 24); }
constexpr std::int32_t insn_sbx(Instruction i) { return static_cast<std::int32_t>(insn_bx(i)); }
constexpr std::uint8_t insn_flags(Instruction i) { return static_cast<std::uint8_t>(i >> 56); }

// One function's register code, constants and line table. Plain data, immutable once compiled
// (notes D8). Lives in ObjFunction next to (and independent of) the stack VM's Chunk.
struct RegChunk {
    // Run-length encoded, like Chunk's: instructions from index `first` up to the next entry's
    // came from source line `line`.
    struct LineRun {
        std::size_t first;
        int line;
    };

    std::vector<Instruction> code;
    std::vector<Value> constants;
    std::vector<LineRun> lines;
    // Registers the function needs: one more than the highest register it ever names, which
    // counts locals, temporaries, and the callee and arguments it places for a call. The VM
    // reserves this many slots when it enters the function.
    int frame_size = 0;

    // Appends and returns the new instruction's index.
    std::size_t emit(Instruction insn, int line);
    // Appends without deduplicating (the compiler does that); returns the index.
    std::size_t add_constant(Value value);
    // Source line of instruction `index` (index must be < code.size()).
    int line_at(std::size_t index) const;
    // Bytes owned by the vectors (capacity, like the heap's accounting).
    std::size_t owned_bytes() const;
};

}  // namespace rung
