#pragma once

#include <memory>
#include <optional>
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
};

// Creates the engine called `name` ("tree", and later "stack", "register", "jit"), or null if
// there is no such engine on this platform. Registers the engine's GC roots with `heap`.
std::unique_ptr<Engine> make_engine(std::string_view name, Heap& heap, Output& out);

// The names make_engine accepts, for usage messages.
std::vector<std::string_view> engine_names();

}  // namespace rung
