#pragma once

#include <cstddef>

#include "ast.h"

namespace rung {

struct FoldStats {
    std::size_t expressions_folded = 0;  // operations replaced by their value or one operand
    std::size_t statements_removed = 0;  // statements dropped as unreachable or never taken
};

// Ladder rung 3f (docs/notes.md D4 row 8): compile-time simplification of a resolved program,
// done in place on the syntax tree so every compiler and the tree-walker see the same result.
// Behind `--fold`; with it off nothing here runs.
//
// Expressions. A unary or binary operation whose operands are literals is replaced by its value,
// computed by the same runtime operations the engines call (src/runtime/ops.h), so wraparound,
// float rules and the printed form agree by construction. An operation that would raise a
// runtime error (`1 / 0`, `1 + true`, `-"a"`) is left alone, so the error still happens at run
// time, on its own line, after the output that precedes it. String operations are not folded
// (a folded `+` would allocate); `!` of a literal, and `and` / `or` whose left side is a
// literal, are, because their result is one of their operands or a bool.
//
// Statements. The statements after a `return` in the same block are dropped, and so are an
// `if` branch that a literal condition rules out and a `while` whose literal condition is
// falsy. A statement is never moved into or out of a block, so the resolver's scope "hops"
// (notes D11) stay valid.
//
// Run it after resolve() and before compiling or running. It does not report errors.
FoldStats fold(Program& program);

}  // namespace rung
