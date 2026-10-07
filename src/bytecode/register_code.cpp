#include "bytecode/register_code.h"

#include <algorithm>
#include <cassert>

namespace rung {

std::size_t RegChunk::emit(Instruction insn, int line) {
    if (lines.empty() || lines.back().line != line) lines.push_back({code.size(), line});
    code.push_back(insn);
    return code.size() - 1;
}

std::size_t RegChunk::add_constant(Value value) {
    constants.push_back(value);
    return constants.size() - 1;
}

int RegChunk::line_at(std::size_t index) const {
    assert(index < code.size() && !lines.empty());
    auto it = std::upper_bound(
        lines.begin(), lines.end(), index,
        [](std::size_t i, const LineRun& run) { return i < run.first; });
    return (it - 1)->line;
}

std::size_t RegChunk::owned_bytes() const {
    return code.capacity() * sizeof(Instruction) + constants.capacity() * sizeof(Value) +
           lines.capacity() * sizeof(LineRun);
}

}  // namespace rung
