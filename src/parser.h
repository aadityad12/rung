#pragma once

#include <memory>
#include <optional>
#include <string>

#include "ast.h"
#include "diagnostic.h"

namespace rung {

struct ParseResult {
    std::unique_ptr<Program> program;  // null on error
    std::optional<CompileError> error;

    bool ok() const { return !error.has_value(); }
};

// The deepest legal nesting of expressions and statements (docs/notes.md §2.6).
constexpr int kMaxNesting = 200;

// The longest legal chain of operations in one expression (docs/notes.md §2.6). Flat chains such
// as `1 + 1 + ... + 1` do not nest, but their syntax tree is as tall as they are long.
constexpr int kMaxChain = 1000;

// Lexes and parses `source`, stopping at the first error (lexer or parser). The Program takes
// ownership of the text so the tokens' string_views stay valid.
ParseResult parse(std::string source);

}  // namespace rung
