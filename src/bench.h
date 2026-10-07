#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine.h"
#include "runtime/heap.h"

namespace rung {

// What bench mode measured (notes D12): the time of each call to the global `run`, in
// nanoseconds, and the printed form of the last call's return value.
struct BenchResult : EngineResult {
    std::vector<long long> iterations_ns;
    std::string result;
};

// Calls the global zero-argument function `run` `iterations` times through the engine interface,
// timing each call with std::chrono::steady_clock. Timing is done here, in C++, around the call,
// rather than by a clock() call inside the Rung program, so the measured function stays free of
// native calls the JIT would have to refuse (notes D5). The engine must already have run the
// program's top level. Stops at the first runtime error.
BenchResult run_bench(Engine& engine, std::size_t iterations);

// Asks macOS to schedule the calling thread on performance cores (notes D5). A no-op elsewhere.
void set_benchmark_thread_qos();

// Renders the JSON file bench mode writes: engine name, per-iteration times, the result, and the
// heap statistics (which are cheap and useful for the allocation-heavy benchmarks). An engine with
// a JIT also reports `jit_compile_ns`, the total time it spent compiling during the whole process
// (top level and every iteration; notes D16), so a speedup can be shown next to what it cost.
std::string bench_json(std::string_view engine_name, const BenchResult& bench,
                       const HeapStats& heap_stats,
                       std::optional<std::uint64_t> jit_compile_ns = std::nullopt);

// Quotes and escapes `text` as a JSON string.
std::string json_quote(std::string_view text);

}  // namespace rung
