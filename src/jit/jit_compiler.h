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

// Where machine code finds an array's elements, as byte offsets from the object's address (the
// pointer in its NaN box). ObjArray keeps its elements in a std::vector, whose layout the C++
// standard does not fix, so it is measured once on a real ObjArray (array_layout) instead of
// assumed. `usable` is false if the vector does not keep plain begin and end pointers that the
// code can reach with one load each; then every array instruction stays in the VM.
struct ArrayLayout {
    bool usable = false;
    std::uint32_t kind = 0;   // the header's ObjKind byte
    std::uint32_t begin = 0;  // the elements' first-element pointer
    std::uint32_t end = 0;    // the one-past-the-last-element pointer
};
const ArrayLayout& array_layout();

// Returns nothing if every instruction is one the JIT compiles: int arithmetic (+ - * / %,
// unary -), the six comparisons, register moves, int / nil / bool constants, jumps, returns, and
// reading or writing one array element (`a[i]`, `a[i] = v`; issue #26, notes D2). Anything that
// touches a string or float constant, creates an array, uses a closure or upvalue, a global, a
// call or `print` rejects the whole function (notes D7: `fib` calls itself through a global, so
// it is rejected; so is `sieve`'s `run`, which calls the global native `array`).
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
