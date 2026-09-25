#pragma once

#include <optional>

#include "ast.h"
#include "diagnostic.h"

namespace rung {

// The most parameters, call arguments, live locals per function, and captured variables per
// function that any engine accepts (docs/notes.md D11). The bytecode uses one-byte operands.
constexpr int kMaxLocalLimit = 255;

// Walks a parsed program once, before any engine runs, and (notes D11):
//   1. reports the static errors listed in docs/notes.md §2.7, stopping at the first;
//   2. fills in `Variable::binding` and `Assign::binding` for every variable use: global, or
//      local with the number of scopes ("hops") to walk up from the use to the declaration.
// Every engine gets its scoping from these bindings, so they agree by construction.
std::optional<CompileError> resolve(Program& program);

}  // namespace rung
