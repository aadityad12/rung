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

// Lexes and parses `source`, stopping at the first error (lexer or parser). The Program takes
// ownership of the text so the tokens' string_views stay valid.
ParseResult parse(std::string source);

}  // namespace rung
