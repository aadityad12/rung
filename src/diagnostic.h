#pragma once

#include <string>

namespace rung {

// A compile-time error: lexing, parsing, or compiling. Every engine reports these identically
// (docs/notes.md §2.4), so the formatting lives in exactly one place.
struct CompileError {
    int line;
    std::string message;
};

// "[line N] compile error: <message>"
std::string format_error(const CompileError& error);

}  // namespace rung
