#include "diagnostic.h"

namespace rung {

std::string format_error(const CompileError& error) {
    return "[line " + std::to_string(error.line) + "] compile error: " + error.message;
}

}  // namespace rung
