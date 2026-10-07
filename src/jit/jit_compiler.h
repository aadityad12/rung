// The baseline JIT's compiler (notes D16): decides whether a function's register bytecode is
// inside the whitelist, and translates it into ARM64 machine code words. It only produces words;
// src/jit/jit.cpp puts them in executable memory and the register VM runs them.
//
// The whitelist check is plain bytecode inspection and is built everywhere, so its unit tests
// run on every platform. Code generation needs the NaN-boxed value layout (its type guards test
// tag bits, notes D3 and D15), so it exists only in RUNG_NANBOX builds; it still compiles on
// x86-64 there, because the emitter only produces bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bytecode/register_code.h"

namespace rung::jit {

// Why a function stays in the register VM: the first instruction outside the whitelist, and
// what it does that the JIT does not.
struct Rejection {
    std::size_t index;
    std::string reason;
};

// The largest frame the JIT compiles. Machine code addresses register r as `[base, #r*8]`, an
// LDR/STR with an unsigned 12-bit offset scaled by 8, which reaches register 4095. Ordinary
// functions use a few dozen registers (notes D14).
constexpr int kMaxFrameSize = 4096;

// Returns nothing if every instruction is one the JIT compiles: int arithmetic (+ - * / %,
// unary -), the six comparisons, register moves, int / nil / bool constants, jumps and returns.
// Anything that touches a string, a float constant, an array, a closure or upvalue, a global, a
// call or `print` rejects the whole function (notes D7: `fib` calls itself through a global, so
// it is rejected).
std::optional<Rejection> check_whitelist(const RegChunk& chunk);

// The opcode a word had before the superinstruction pass: a fused word's first half, else the
// word's own opcode. The JIT compiles fused code as the unfused instructions (notes D16).
RegOp unfused_op(Instruction insn);

#if RUNG_NANBOX

// Machine code for one function, as instruction words.
struct GeneratedCode {
    std::vector<std::uint32_t> words;
    // starts[i] is the index in `words` of the first machine instruction generated for bytecode
    // instruction i: the map from machine code back to bytecode (notes D3). Every exit from the
    // code carries its bytecode index in w0 as well, so the VM needs no lookup at run time.
    std::vector<std::size_t> starts;
};

// Translates `chunk`, which must have passed check_whitelist. The function's calling
// convention and the bail-out protocol are described in jit_compiler.cpp and notes D16. Throws
// arm64::EmitError if the code cannot be encoded (a branch beyond B.cond's +-1 MiB reach).
GeneratedCode generate(const RegChunk& chunk);

#endif  // RUNG_NANBOX

}  // namespace rung::jit
