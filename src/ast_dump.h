#pragma once

#include <string>

#include "ast.h"

namespace rung {

// S-expression form of a program for `--dump-ast` and the parser tests: one top-level
// statement per line, each ending in '\n'. The format is described in ast_dump.cpp.
std::string dump_ast(const Program& program);

}  // namespace rung
