#include "superinstructions.h"

#include <vector>

namespace rung {

std::size_t fuse_superinstructions(RegChunk& chunk) {
    std::size_t fused = 0;
    std::vector<Instruction>& code = chunk.code;
    std::size_t index = 0;
    while (index + 1 < code.size()) {
        RegOp first = insn_op(code[index]);
        if (is_fused(first)) {
            // Already fused (the pass was run twice): its next word is its second half, and must
            // not become the first half of a pair, or the VM would read it as the wrong thing.
            index += 2;
            continue;
        }
        RegOp fused_op = fuse_pair(first, insn_op(code[index + 1]));
        if (fused_op == first) {
            ++index;
            continue;
        }
        // Only the opcode byte changes; the operands stay where the compiler put them.
        code[index] = (code[index] & ~Instruction{0xFF}) | static_cast<Instruction>(fused_op);
        ++fused;
        index += 2;  // the second word is now this instruction's operand, not a new first half
    }
    return fused;
}

}  // namespace rung
