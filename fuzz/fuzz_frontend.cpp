// libFuzzer target for the front end: lexer, parser, resolver (issue #12).
//
// It deliberately stops before execution. Rung programs may loop forever (`while (true) {}` is
// legal), so running a fuzz input would "hang" on valid programs and drown out real bugs. The
// front end, by contrast, must terminate on every input: it either produces an AST or a
// compile error. It must also never crash, leak, or trip ASan/UBSan (notes D9, D11, §2.6).

#include <cstddef>
#include <cstdint>
#include <string>

#include "ast_dump.h"
#include "diagnostic.h"
#include "lexer.h"
#include "parser.h"
#include "resolver.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // The lexer's tokens point into the source text, so it must outlive them.
    const std::string source(reinterpret_cast<const char*>(data), size);

    // The lexer on its own: it has error paths the parser never reaches once it stops early.
    rung::LexResult lexed = rung::lex(source);
    if (!lexed.ok()) (void)rung::format_error(*lexed.error);

    // Lexer + parser, as every engine sees them.
    rung::ParseResult parsed = rung::parse(source);
    if (!parsed.ok()) {
        (void)rung::format_error(*parsed.error);
        return 0;
    }

    // Static errors and variable bindings (notes D11).
    if (auto error = rung::resolve(*parsed.program)) {
        (void)rung::format_error(*error);
        return 0;
    }

    // Printing the resolved AST walks every node and reads every binding, so it doubles as a
    // check that the resolver left the tree fully annotated.
    (void)rung::dump_ast(*parsed.program);
    return 0;
}
