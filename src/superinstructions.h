#pragma once

#include <cstddef>

#include "bytecode/register_code.h"

namespace rung {

// The peephole pass behind `--superinstructions` (ladder rung 3d, notes §5). Walks one function's
// register code from the start and, wherever two adjacent instructions are a pair listed in
// kFusedPairs, changes the opcode of the first to the pair's fused opcode (see "Fusion" in
// bytecode/register_code.h). Nothing else changes: no word moves, so jump offsets, the line table
// and the inline cache need no repair. A pair is taken left to right and never overlaps the next
// one (the second word of a pair is not the first of another), so `a b c` with both `a b` and
// `b c` fusable fuses `a b` only. Returns the number of pairs fused.
//
// Run once per function, after the function's code is complete.
std::size_t fuse_superinstructions(RegChunk& chunk);

}  // namespace rung
