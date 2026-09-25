#include "bytecode/chunk.h"

#include <algorithm>
#include <cassert>

namespace rung {

void Chunk::emit_byte(std::uint8_t byte, int line) {
    if (lines.empty() || lines.back().line != line) lines.push_back({code.size(), line});
    code.push_back(byte);
}

void Chunk::emit_u24(std::uint32_t value, int line) {
    assert(value <= kMaxU24);
    emit_byte(static_cast<std::uint8_t>(value >> 16), line);
    emit_byte(static_cast<std::uint8_t>(value >> 8), line);
    emit_byte(static_cast<std::uint8_t>(value), line);
}

std::size_t Chunk::add_constant(Value value) {
    constants.push_back(value);
    return constants.size() - 1;
}

int Chunk::line_at(std::size_t offset) const {
    assert(offset < code.size() && !lines.empty());
    // The run that contains `offset` is the last one that starts at or before it.
    auto it = std::upper_bound(
        lines.begin(), lines.end(), offset,
        [](std::size_t off, const LineRun& run) { return off < run.offset; });
    return (it - 1)->line;
}

std::size_t Chunk::owned_bytes() const {
    return code.capacity() + constants.capacity() * sizeof(Value) +
           lines.capacity() * sizeof(LineRun);
}

}  // namespace rung
