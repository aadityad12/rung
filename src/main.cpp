#include <pthread.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast_dump.h"
#include "compiler_stack.h"
#include "disassembler.h"
#include "engine.h"
#include "lexer.h"
#include "output.h"
#include "parser.h"
#include "resolver.h"
#include "runtime/heap.h"

namespace {

// Exit codes from sysexits.h (docs/notes.md D9).
constexpr int kExitOk = 0;
constexpr int kExitUsage = 64;
constexpr int kExitCompileError = 65;
constexpr int kExitNoInput = 66;
constexpr int kExitRuntimeError = 70;

// The engine runs on a thread with this much stack (notes D12): the tree-walker recurses on the
// C++ stack, and 10,000 nested Rung calls must fit even under AddressSanitizer's big frames.
constexpr std::size_t kEngineStackBytes = std::size_t{512} * 1024 * 1024;

struct Options {
    bool want_tokens = false;
    bool want_ast = false;
    bool want_bytecode = false;
    bool gc_stress = false;
    bool want_stats = false;
    std::string engine = "tree";
    const char* path = nullptr;
};

void print_usage() {
    std::cerr << "usage: rung [--engine=";
    const char* separator = "";
    for (std::string_view name : rung::engine_names()) {
        std::cerr << separator << name;
        separator = "|";
    }
    std::cerr << "] [--gc-stress] [--stats]\n"
                 "            [--dump-tokens | --dump-ast | --dump-bytecode] <file.rg>\n";
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

// Heap statistics for --stats (notes D10), to stderr so they never mix with program output.
void print_stats(const rung::Heap& heap) {
    rung::HeapStats stats = heap.stats();
    std::cerr << "heap: " << stats.objects_allocated << " objects allocated, "
              << stats.bytes_allocated << " bytes allocated, " << stats.collections
              << " collections, " << stats.peak_live_bytes << " peak heap bytes\n";
}

// Everything after the command line has been understood. Returns the process exit code.
int execute(const Options& options) {
    std::optional<std::string> source = read_file(options.path);
    if (!source) {
        std::cerr << "rung: cannot read '" << options.path << "'\n";
        return kExitNoInput;
    }

    if (options.want_tokens) {
        rung::LexResult lexed = rung::lex(*source);
        if (!lexed.ok()) {
            std::cerr << rung::format_error(*lexed.error) << "\n";
            return kExitCompileError;
        }
        dump_tokens(lexed.tokens);
        if (!options.want_ast) return kExitOk;
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

    if (options.want_ast) std::cout << rung::dump_ast(*parsed.program);

    if (options.want_bytecode) {
        rung::Heap heap;
        rung::StackCompileResult compiled = rung::compile_stack(*parsed.program, heap);
        if (!compiled.ok()) {
            std::cerr << rung::format_error(*compiled.error) << "\n";
            return kExitCompileError;
        }
        std::cout << rung::disassemble(*compiled.function);
    }
    if (options.want_ast || options.want_bytecode) return kExitOk;

    // Declaration order matters: the engine must be destroyed before the heap it registered
    // roots with, and `out` (which flushes when destroyed) outlives the engine's last print.
    rung::Heap heap;
    heap.set_stress(options.gc_stress);
    rung::Output out;
    std::unique_ptr<rung::Engine> engine = rung::make_engine(options.engine, heap, out);

    rung::EngineResult result = engine->run(*parsed.program);

    int exit_code = kExitOk;
    // Buffered program output first, so stdout and stderr appear in the order they happened.
    out.flush();
    if (result.compile_error) {
        std::cerr << rung::format_error(*result.compile_error) << "\n";
        exit_code = kExitCompileError;
    } else if (result.runtime_error) {
        std::cerr << rung::format_runtime_error(*result.runtime_error) << "\n";
        exit_code = kExitRuntimeError;
    }
    if (options.want_stats) print_stats(heap);
    return exit_code;
}

struct ThreadJob {
    const Options* options;
    int exit_code;
};

void* thread_main(void* arg) {
    auto* job = static_cast<ThreadJob*>(arg);
    job->exit_code = execute(*job->options);
    return nullptr;
}

// Runs execute() on a thread with a 512 MiB stack. If the thread cannot be made (a tight
// address-space limit, say) it runs on the main thread rather than refusing to run at all.
int execute_on_big_stack(const Options& options) {
    ThreadJob job{&options, kExitOk};
    pthread_attr_t attr;
    pthread_t thread;
    bool started = false;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, kEngineStackBytes) == 0) {
            started = pthread_create(&thread, &attr, thread_main, &job) == 0;
        }
        pthread_attr_destroy(&attr);
    }
    if (!started) return execute(options);
    pthread_join(thread, nullptr);
    return job.exit_code;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    constexpr std::string_view kEnginePrefix = "--engine=";

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--dump-tokens") {
            options.want_tokens = true;
        } else if (arg == "--dump-ast") {
            options.want_ast = true;
        } else if (arg == "--dump-bytecode") {
            options.want_bytecode = true;
        } else if (arg == "--gc-stress") {
            options.gc_stress = true;
        } else if (arg == "--stats") {
            options.want_stats = true;
        } else if (arg.substr(0, kEnginePrefix.size()) == kEnginePrefix) {
            options.engine = std::string(arg.substr(kEnginePrefix.size()));
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "rung: unknown option '" << arg << "'\n";
            print_usage();
            return kExitUsage;
        } else if (options.path == nullptr) {
            options.path = argv[i];
        } else {
            print_usage();
            return kExitUsage;
        }
    }
    if (options.path == nullptr) {
        print_usage();
        return kExitUsage;
    }
    std::vector<std::string_view> engines = rung::engine_names();
    if (std::find(engines.begin(), engines.end(), options.engine) == engines.end()) {
        std::cerr << "rung: unknown engine '" << options.engine << "'\n";
        print_usage();
        return kExitUsage;
    }

    return execute_on_big_stack(options);
}
