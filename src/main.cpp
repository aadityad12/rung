#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "lexer.h"

namespace {

// Exit codes from sysexits.h (docs/notes.md D9).
constexpr int kExitUsage = 64;
constexpr int kExitCompileError = 65;
constexpr int kExitNoInput = 66;

void print_usage() {
    std::cerr << "usage: rung [--dump-tokens] <file.rg>\n";
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
    const char* path = nullptr;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--dump-tokens") {
            want_tokens = true;
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

    rung::LexResult lexed = rung::lex(*source);
    if (!lexed.ok()) {
        std::cerr << rung::format_error(*lexed.error) << "\n";
        return kExitCompileError;
    }

    if (want_tokens) {
        dump_tokens(lexed.tokens);
        return 0;
    }

    std::cerr << "rung: no execution engine yet; try --dump-tokens\n";
    return 1;
}
