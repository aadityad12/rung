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

// A runtime error. The operations in src/runtime/ produce only the message (notes §2.5); the
// engine that was running attaches the line of the operator, call's `(`, or index's `[` that
// failed, so no runtime code ever needs to know about lines.
struct RuntimeError {
    int line;
    std::string message;
};

// "[line N] runtime error: <message>"
std::string format_runtime_error(const RuntimeError& error);

}  // namespace rung
