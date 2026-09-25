#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ast_dump.h"
#include "compiler_reg.h"
#include "compiler_stack.h"
#include "disassembler.h"
#include "lexer.h"
#include "parser.h"
#include "resolver.h"
#include "runtime/heap.h"

namespace {

// Exit codes from sysexits.h (docs/notes.md D9).
constexpr int kExitUsage = 64;
constexpr int kExitCompileError = 65;
constexpr int kExitNoInput = 66;

void print_usage() {
    std::cerr << "usage: rung [--engine=stack|register] [--dump-tokens] [--dump-ast] "
                 "[--dump-bytecode] <file.rg>\n";
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void dump_tokens(const std::vector<rung::Token>& tokens) {
    for (const rung::Token& token : tokens) {
        std::printf("%4d  %-13.*s %.*s\n", token.line,
                    static_cast<int>(rung::token_kind_name(token.kind).size()),
                    rung::token_kind_name(token.kind).data(),
                    static_cast<int>(token.lexeme.size()), token.lexeme.data());
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool want_tokens = false;
    bool want_ast = false;
    bool want_bytecode = false;
    // Only --dump-bytecode looks at the engine so far: it picks which compiler's bytecode to
    // print. The full --engine option (notes D12) arrives with the engines.
    std::string_view engine = "stack";
    const char* path = nullptr;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--dump-tokens") {
            want_tokens = true;
        } else if (arg == "--dump-ast") {
            want_ast = true;
        } else if (arg == "--dump-bytecode") {
            want_bytecode = true;
        } else if (arg.substr(0, 9) == "--engine=") {
            engine = arg.substr(9);
            if (engine != "stack" && engine != "register") {
                std::cerr << "rung: unknown or unsupported engine '" << engine << "'\n";
                print_usage();
                return kExitUsage;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "rung: unknown option '" << arg << "'\n";
            print_usage();
            return kExitUsage;
        } else if (path == nullptr) {
            path = argv[i];
        } else {
            print_usage();
            return kExitUsage;
        }
    }
    if (path == nullptr) {
        print_usage();
        return kExitUsage;
    }

    std::optional<std::string> source = read_file(path);
    if (!source) {
        std::cerr << "rung: cannot read '" << path << "'\n";
        return kExitNoInput;
    }

    if (want_tokens) {
        rung::LexResult lexed = rung::lex(*source);
        if (!lexed.ok()) {
            std::cerr << rung::format_error(*lexed.error) << "\n";
            return kExitCompileError;
        }
        dump_tokens(lexed.tokens);
        if (!want_ast) return 0;
    }

    // Parsing (which lexes first) reports lexer and parser errors identically.
    rung::ParseResult parsed = rung::parse(std::move(*source));
    if (!parsed.ok()) {
        std::cerr << rung::format_error(*parsed.error) << "\n";
        return kExitCompileError;
    }

    // Static errors and variable bindings, once, before any engine (notes D11).
    if (std::optional<rung::CompileError> error = rung::resolve(*parsed.program)) {
        std::cerr << rung::format_error(*error) << "\n";
        return kExitCompileError;
    }

    if (want_ast) std::cout << rung::dump_ast(*parsed.program);

    if (want_bytecode) {
        rung::Heap heap;
        if (engine == "register") {
            rung::RegCompileResult compiled = rung::compile_register(*parsed.program, heap);
            if (!compiled.ok()) {
                std::cerr << rung::format_error(*compiled.error) << "\n";
                return kExitCompileError;
            }
            std::cout << rung::disassemble_register(*compiled.function);
        } else {
            rung::StackCompileResult compiled = rung::compile_stack(*parsed.program, heap);
            if (!compiled.ok()) {
                std::cerr << rung::format_error(*compiled.error) << "\n";
                return kExitCompileError;
            }
            std::cout << rung::disassemble(*compiled.function);
        }
    }
    if (want_ast || want_bytecode) return 0;

    std::cerr << "rung: no execution engine yet; try --dump-tokens, --dump-ast or "
                 "--dump-bytecode\n";
    return 1;
}
