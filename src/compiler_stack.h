#pragma once

#include <optional>

#include "ast.h"
#include "diagnostic.h"
#include "runtime/function.h"
#include "runtime/heap.h"

namespace rung {

struct StackCompileResult {
    ObjFunction* function = nullptr;  // the top-level script; null on error
    std::optional<CompileError> error;

    bool ok() const { return !error.has_value(); }
};

// Compiles a resolved program (the resolver must have run: variable uses are read from their
// bindings, notes D11) into stack-VM bytecode. The top level becomes a script function (no
// name, arity 0) whose constants hold every nested function.
//
// The resolver has already rejected invalid programs, so the only errors left are functions
// too large for 24-bit operands (over 16 million constants or 16 MiB of code in one function),
// which no reasonable program reaches.
//
// GC: allocates on `heap`. While compiling, the functions under construction are kept alive by
// a root marker; it is removed before returning. The returned function is NOT rooted: the
// caller must store it somewhere the collector can see (an engine root, a Heap::TempRoot)
// before the next allocation. The Program does not need to outlive the result.
StackCompileResult compile_stack(const Program& program, Heap& heap);

}  // namespace rung
