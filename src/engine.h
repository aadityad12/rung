#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ast.h"
#include "diagnostic.h"
#include "output.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace rung {

// How a run ended: fine, with a compile error the engine found while preparing the program (the
// bytecode engines can hit size limits the resolver cannot see), or with a runtime error.
// At most one of the two errors is set.
struct EngineResult {
    std::optional<CompileError> compile_error;
    std::optional<RuntimeError> runtime_error;

    bool ok() const { return !compile_error && !runtime_error; }
};

// The result of calling a global function from C++. `value` is meaningful only when ok(), and
// it is NOT rooted: read or print it before anything allocates on the heap again.
struct CallResult : EngineResult {
    Value value = make_nil();
};

// The interface every execution engine implements (notes D12). An engine owns nothing but its
// own state: the Heap and Output are the caller's and must outlive it, and so must the Program
// passed to run(), because the engine's functions point into the syntax tree.
class Engine {
public:
    virtual ~Engine() = default;

    virtual std::string_view name() const = 0;

    // Runs a program that the resolver has already accepted (notes D11): defines its globals
    // and executes its top-level statements. Output goes through the Output given at
    // construction; the caller flushes it before writing to stderr.
    virtual EngineResult run(const Program& program) = 0;

    // Calls the global function `name` with no arguments, as bench mode needs (notes D12).
    // Must come after run(). If `name` is not a callable global the error is an ordinary runtime
    // error, reported on line 0 because no Rung source line is involved.
    virtual CallResult call_global(std::string_view name) = 0;

    // Engine-specific lines for `--stats`, printed to stderr after the heap statistics (notes
    // D5). Empty for an engine with nothing to add.
    virtual std::string stats_report() const { return {}; }

    // The most frequent pairs of opcodes dispatched back to back, for `--stats=pairs` (ladder
    // rung 3d). Empty for an engine that does not count them (the tree-walker has no opcodes).
    virtual std::string pair_report() const { return {}; }
    // Total time the JIT has spent compiling, for bench mode's JSON (notes D16); nothing for an
    // engine without a JIT.
    virtual std::optional<std::uint64_t> jit_compile_ns() const { return std::nullopt; }
};

// Switches that change how an engine runs a program without changing what it computes. Each is
// a ladder rung (notes D4), and an engine that does not have the rung refuses it.
struct EngineOptions {
    // Ladder rung 3e: GET_GLOBAL and SET_GLOBAL remember their global's storage cell. Register
    // VM and JIT (which is the register VM plus machine code) only.
    bool inline_cache = false;
    // Ladder rung 3d: the register compiler fuses adjacent instruction pairs into
    // superinstructions. Register VM and JIT only.
    bool superinstructions = false;
    // Calls plus loop back-edges after which a function is compiled (--jit-threshold, notes D16).
    // Read only by --engine=jit.
    std::uint32_t jit_threshold = 1000;
    // Where --jit-log writes each compile, rejection and bail-out; null for no log.
    std::FILE* jit_log = nullptr;
    // --jit-background (Engine 5, notes D8): compile hot functions on a second thread while the
    // VM keeps running them, instead of pausing to compile. Read only by --engine=jit.
    bool jit_background = false;
};

// Creates the engine called `name` ("tree", "stack", "register", or "jit" where the JIT is
// built), or null if there is no such engine on this platform, or the engine lacks an option that
// is set. Registers the engine's GC roots with `heap`.
std::unique_ptr<Engine> make_engine(std::string_view name, Heap& heap, Output& out,
                                    const EngineOptions& options = {});

// The names make_engine accepts in this build, for usage messages.
std::vector<std::string_view> engine_names();

// Whether this build has the JIT (--engine=jit): it needs an arm64 CPU and the NaN-boxed value
// layout (notes D3, D6, D16).
constexpr bool jit_available() {
#if RUNG_JIT
    return true;
#else
    return false;
#endif
}

}  // namespace rung
