#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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
    // How often opcode X was dispatched immediately after opcode Y (`--stats=pairs`, ladder rung
    // 3d): the entry for "X after Y" is pairs[Y * kOpCount + X]. The extra last row, with
    // Y = kOpCount, is "nothing before it" (the first instruction a run dispatches), so the update
    // needs no branch. "Immediately after" is in the order instructions are dispatched, so it
    // includes a jump and its target, and a call and the callee's first instruction.
    std::array<std::uint64_t, (kOpCount + 1) * kOpCount> pairs{};
    std::size_t previous_op = kOpCount;
    std::uint64_t rung_calls = 0;    // calls that pushed a frame
    std::uint64_t native_calls = 0;  // calls to array, len and clock
    // Register VM with --inline-cache (ladder rung 3e): GET_GLOBAL / SET_GLOBAL executions that
    // used the remembered cell, and those that had to look the name up. Both stay 0 otherwise.
    std::uint64_t global_cache_hits = 0;
    std::uint64_t global_cache_misses = 0;

    std::uint64_t instructions() const {
        std::uint64_t total = 0;
        for (std::uint64_t n : by_opcode) total += n;
        return total;
    }

    // Called once per dispatched instruction.
    void count_dispatch(std::size_t op) {
        ++by_opcode[op];
        ++pairs[previous_op * kOpCount + op];
        previous_op = op;
    }
    // A new run starts: its first instruction does not follow the last one of the previous run.
    void restart_pairs() { previous_op = kOpCount; }

    // The `count` most frequent pairs, one line each: the two opcode names, how often, and the
    // share of all dispatches. `name` maps an opcode number to its text. Ties are ordered by
    // opcode number, so the output is the same on every run.
    template <typename NameFn>
    std::string pair_report(NameFn name, std::size_t count = 30) const {
        std::vector<std::pair<std::uint64_t, std::size_t>> found;  // (count, index in pairs)
        std::uint64_t total = 0;
        for (std::size_t first = 0; first < kOpCount; ++first) {
            for (std::size_t second = 0; second < kOpCount; ++second) {
                std::uint64_t n = pairs[first * kOpCount + second];
                if (n == 0) continue;
                found.emplace_back(n, first * kOpCount + second);
                total += n;
            }
        }
        std::sort(found.begin(), found.end(), [](const auto& x, const auto& y) {
            return x.first != y.first ? x.first > y.first : x.second < y.second;
        });
        std::string text = "pairs: " + std::to_string(total) + " opcode pairs dispatched, " +
                           std::to_string(found.size()) + " distinct\n";
        for (std::size_t i = 0; i < found.size() && i < count; ++i) {
            std::size_t first = found[i].second / kOpCount;
            std::size_t second = found[i].second % kOpCount;
            char row[160];
            std::snprintf(row, sizeof row, "  %-14s -> %-14s %12llu  %5.1f%%\n", name(first),
                          name(second), static_cast<unsigned long long>(found[i].first),
                          100.0 * static_cast<double>(found[i].first) / static_cast<double>(total));
            text += row;
        }
        return text;
    }
};

}  // namespace rung
