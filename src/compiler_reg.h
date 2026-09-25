#pragma once

#include <optional>

#include "ast.h"
#include "diagnostic.h"
#include "runtime/function.h"
#include "runtime/heap.h"

namespace rung {

struct RegCompileResult {
    ObjFunction* function = nullptr;  // the top-level script; null on error
    std::optional<CompileError> error;

    bool ok() const { return !error.has_value(); }
};

// Compiles a resolved program (the resolver must have run, notes D11) into register-VM
// bytecode (notes D14, format in bytecode/register_code.h). The top level becomes a script
// function (no name, arity 0) whose constants hold every nested function. The code is in each
// function's `reg` member; its stack-VM `chunk` stays empty.
//
// The resolver has already rejected invalid programs. The 16-bit register operands are wide
// enough for anything it accepts (at most 255 locals, parameters and arguments, and expressions
// nested at most 200 deep), so the only errors left are the same "function too large" ones the
// stack compiler has, for functions that need more than 2^31 instructions or more than 65,535
// registers, which no accepted program reaches.
//
// GC: exactly as compile_stack: functions under construction are rooted by a marker that is
// removed before returning, and the returned function is NOT rooted.
RegCompileResult compile_register(const Program& program, Heap& heap);

}  // namespace rung
