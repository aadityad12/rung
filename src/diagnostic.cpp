#include "diagnostic.h"

namespace rung {

std::string format_error(const CompileError& error) {
    return "[line " + std::to_string(error.line) + "] compile error: " + error.message;
}

std::string format_runtime_error(const RuntimeError& error) {
    return "[line " + std::to_string(error.line) + "] runtime error: " + error.message;
}

}  // namespace rung
