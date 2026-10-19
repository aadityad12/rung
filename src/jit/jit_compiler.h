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

namespace rung {
struct ObjFunction;
}

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

// Where machine code finds a closure's function: a byte offset from the closure's address.
// Measured once on a real ObjClosure, like array_layout. `usable` is false if the pointer is not
// found exactly once at an offset one LDR reaches; then no function calls itself in machine code.
struct ClosureLayout {
    bool usable = false;
    std::uint32_t kind = 0;      // the header's ObjKind byte
    std::uint32_t function = 0;  // the ObjFunction pointer
};
const ClosureLayout& closure_layout();

// Self-recursion (issue #25, notes D7). A function may call itself through the global that has
// its name: `GET_GLOBAL r, "fib"` then `CALL r`. The machine code pushes the callee's frame onto
// the VM's own frame stack, so if a check fails in a nested call the VM can resume exactly where
// it failed. These describe the VM's frame for the machine code; vm_reg.h checks them against
// RegisterEngine::CallFrame with static_asserts.
constexpr std::uint32_t kFrameBytes = 24;          // sizeof(CallFrame)
constexpr std::uint32_t kFrameClosureOffset = 0;   // ObjClosure* closure
constexpr std::uint32_t kFramePcOffset = 8;        // const Instruction* pc
constexpr std::uint32_t kFrameBaseOffset = 16;     // Value* base
// Returned instead of a bytecode index when a check failed in a nested call. The innermost
// failing frame's pc is already saved in its CallFrame, and every frame between it and the one
// the VM entered has its pc after its CALL, so the VM just reloads the newest frame. No function
// has 2^32 - 1 instructions, so it cannot be mistaken for an index.
constexpr std::uint32_t kNestedBailout = 0xFFFF'FFFF;
// The largest frame of a function that calls itself. After a call returns, the caller reads the
// result from the callee's window, whose registers start A + 1 above its own: register A + 1 + B
// must still be in one LDR's reach (4095), so both frames are kept below half of it.
constexpr int kMaxSelfCallFrameSize = 2047;

// What the compiler needs to know to compile a self-call, filled in by the JIT from the
// function. Without it (null), globals and calls are rejected as before.
struct SelfCall {
    const ObjFunction* function;  // the function being compiled
    const Value* global;          // the cell of the global with its name, or null
};

// Returns nothing if every instruction is one the JIT compiles: int arithmetic (+ - * / %,
// unary -), the six comparisons, register moves, int / nil / bool constants, jumps, returns, and
// reading or writing one array element (`a[i]`, `a[i] = v`; issue #26, notes D2), and, given
// `self`, a call to itself through its own global (issue #25). Anything that touches a string
// or float constant, creates an array, uses a closure or upvalue, any other global, any other
// call or `print` rejects the whole function (so `sieve`'s `run`, which calls the global native
// `array`, is rejected).
std::optional<Rejection> check_whitelist(const RegChunk& chunk, const SelfCall* self = nullptr);

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
GeneratedCode generate(const RegChunk& chunk, const SelfCall* self = nullptr);

#endif  // RUNG_NANBOX

}  // namespace rung::jit
