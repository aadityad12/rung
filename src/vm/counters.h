#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// RUNG_VM_COUNTERS is set by CMake (ON in the debug, asan and tsan presets, OFF in release).
// When it is 0 the counting code is not compiled at all (notes D5).
#ifndef RUNG_VM_COUNTERS
#define RUNG_VM_COUNTERS 0
#endif

namespace rung {

// What a VM counted while it ran (notes D5): instructions dispatched per opcode, and calls.
// `kOpCount` is the size of the VM's opcode enum, so the stack VM and the register VM share the
// struct and its report format. The struct always exists so an engine's layout does not depend
// on the build flag; only the code that updates it does.
template <std::size_t kOpCount>
struct BasicVmCounters {
    std::array<std::uint64_t, kOpCount> by_opcode{};
    std::uint64_t rung_calls = 0;    // calls that pushed a frame
    std::uint64_t native_calls = 0;  // calls to array, len and clock

    std::uint64_t instructions() const {
        std::uint64_t total = 0;
        for (std::uint64_t n : by_opcode) total += n;
        return total;
    }
};

}  // namespace rung
