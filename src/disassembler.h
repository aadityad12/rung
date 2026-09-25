#pragma once

#include <cstddef>
#include <string>

#include "bytecode/chunk.h"
#include "runtime/function.h"

namespace rung {

// Human-readable bytecode for `--dump-bytecode` and the compiler tests. Every function gets a
// header line, then one row per instruction:
//
//   == add (arity 2, upvalues 0) ==
//   0000    3 GET_LOCAL      1
//   0002    3 GET_LOCAL      2
//   0004    3 ADD
//
// A row is: the instruction's byte offset, its source line, the opcode name, then its
// operands. Constants are shown by value after their index (strings quoted), and jumps show
// the absolute offset they land on, as `-> 0021`. A CLOSURE row is followed by one row per
// captured variable (`local` = a slot of the enclosing function, `upvalue` = one of the
// enclosing closure's own upvalues).
//
// disassemble() prints `script` first, then every function nested inside it, depth first, in
// the order they appear in the constant pools, separated by a blank line.
std::string disassemble(const ObjFunction& script);

// Just one function (header and rows), without its nested functions.
std::string disassemble_function(const ObjFunction& function);

// Appends the row(s) for the instruction at `offset` and returns the offset of the next
// instruction.
std::size_t disassemble_instruction(const Chunk& chunk, std::size_t offset, std::string& out);

// Register bytecode (compile_register) in the same style. Rows show the instruction's index
// (not a byte offset: every instruction is one 64-bit word), its source line, the opcode name and
// its operands: `r3` is register 3, `k2(7)` is constant 2 (shown by value), and a jump shows the
// absolute index it lands on, as `-> 0021`. The header adds the function's frame size:
//
//   == add (arity 2, upvalues 0, frame 3) ==
//   0000    3 ADD            r2 r0 r1
//   0001    3 RETURN         r2
//
// A CLOSURE row is followed by one row per Capture word (`local N` = register N of the
// enclosing function, `upvalue N` = one of the enclosing closure's own upvalues).
std::string disassemble_register(const ObjFunction& script);
std::string disassemble_register_function(const ObjFunction& function);
std::size_t disassemble_register_instruction(const RegChunk& chunk, std::size_t index,
                                             std::string& out);

}  // namespace rung
