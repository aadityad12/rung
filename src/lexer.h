#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "diagnostic.h"
#include "token.h"

namespace rung {

struct LexResult {
    std::vector<Token> tokens;  // on success, always ends with an Eof token
    std::optional<CompileError> error;

    bool ok() const { return !error.has_value(); }
};

// Turns source text into tokens in a single pass, stopping at the first error.
// Tokens hold string_views into `source`, so `source` must outlive the result.
LexResult lex(std::string_view source);

}  // namespace rung
