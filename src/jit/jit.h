// The baseline JIT (Engine 4, notes D16): owns the machine code of every function it compiled,
// and keeps the numbers --jit-log and --stats report. The register VM decides *when* to compile
// (hotness) and runs the code; this class decides *whether* a function can be compiled and
// does it.
//
// Built only where the JIT exists: an arm64 CPU (the code is ARM64) and the NaN-boxed value
// layout (its type guards test NaN-box tags, notes D3). CMake defines RUNG_JIT there.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "jit/exec_memory.h"
#include "runtime/function.h"

namespace rung::jit {

class Jit {
public:
    // `log` is where --jit-log writes (stderr from the CLI), or null for no log.
    Jit(std::uint32_t threshold, std::FILE* log);

    // Calls plus loop back-edges after which a function is compiled (--jit-threshold).
    std::uint32_t threshold() const { return threshold_; }

    // Compiles `function`'s register code. On success sets jit_entry and status Compiled; if the
    // function is outside the whitelist, sets status Rejected and the VM keeps running it.
    // Either way it is never tried again.
    void compile(ObjFunction& function);

    // The VM reports every exit from machine code that was not a return: a failed type guard
    // or a zero divisor in the instruction at `index`.
    void note_bailout(const ObjFunction& function, std::size_t index);

    // One line for --stats.
    std::string stats_report() const;
    // Wall-clock time spent compiling, every function together (whitelist check, code
    // generation, mapping and writing executable memory).
    std::uint64_t total_compile_ns() const { return compile_ns_; }
    std::uint64_t functions_compiled() const { return compiled_; }
    std::uint64_t functions_rejected() const { return rejected_; }
    std::uint64_t bailouts() const { return bailouts_; }

private:
    std::uint32_t threshold_;
    std::FILE* log_;
    // One mapping per compiled function. Never freed before the JIT is, even if the function
    // object is collected, so a jit_entry can never dangle while the engine runs.
    std::vector<ExecBuffer> buffers_;
    std::uint64_t compiled_ = 0;
    std::uint64_t rejected_ = 0;
    std::uint64_t bailouts_ = 0;
    std::uint64_t code_bytes_ = 0;
    std::uint64_t compile_ns_ = 0;
};

}  // namespace rung::jit
