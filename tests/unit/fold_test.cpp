#include <doctest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ast_dump.h"
#include "compiler_reg.h"
#include "compiler_stack.h"
#include "disassembler.h"
#include "engine.h"
#include "fold.h"
#include "output.h"
#include "parser.h"
#include "resolver.h"
#include "runtime/heap.h"

namespace {

std::unique_ptr<rung::Program> resolved(const std::string& source) {
    rung::ParseResult parsed = rung::parse(source);
    REQUIRE_MESSAGE(parsed.ok(), rung::format_error(*parsed.error));
    auto error = rung::resolve(*parsed.program);
    REQUIRE_MESSAGE(!error.has_value(), rung::format_error(*error));
    return std::move(parsed.program);
}

std::string without_final_newline(std::string text) {
    if (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

// The --dump-ast text of `source` after resolving and folding.
std::string folded(const std::string& source, rung::FoldStats* stats = nullptr) {
    std::unique_ptr<rung::Program> program = resolved(source);
    rung::FoldStats result = rung::fold(*program);
    if (stats != nullptr) *stats = result;
    return without_final_newline(rung::dump_ast(*program));
}

// The same without folding, for tests that say "this stays as it was".
std::string unfolded(const std::string& source) {
    return without_final_newline(rung::dump_ast(*resolved(source)));
}

// Every expression below is statement-level `print`, so the dump is "(print <expr>)".
std::string print_of(const std::string& expr) { return folded("print " + expr + ";"); }

struct Run {
    std::string output;
    std::string error;  // "[line N] runtime error: ..." or empty
};

// Runs `source` on `engine_name`, folded or not.
Run run_on(const std::string& engine_name, const std::string& source, bool fold) {
    std::unique_ptr<rung::Program> program = resolved(source);
    if (fold) rung::fold(*program);
    std::FILE* file = std::tmpfile();
    Run run;
    {
        rung::Heap heap;
        rung::Output out(file);
        std::unique_ptr<rung::Engine> engine = rung::make_engine(engine_name, heap, out);
        REQUIRE(engine != nullptr);
        rung::EngineResult result = engine->run(*program);
        out.flush();
        if (result.runtime_error) run.error = rung::format_runtime_error(*result.runtime_error);
    }
    std::rewind(file);
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) run.output.append(buffer, n);
    std::fclose(file);
    return run;
}

}  // namespace

TEST_CASE("fold: integer arithmetic uses the runtime operations") {
    CHECK(print_of("1 + 2 * 3") == "(print 7)");
    CHECK(print_of("10 - 4 - 3") == "(print 3)");
    // Wraparound (notes D1): the same answer the VM gives at run time.
    CHECK(print_of("2147483647 + 1") == "(print -2147483648)");
    CHECK(print_of("-2147483648 - 1") == "(print 2147483647)");
    CHECK(print_of("65536 * 65536") == "(print 0)");
    CHECK(print_of("-(-2147483648)") == "(print -2147483648)");
    // Division truncates toward zero, % takes the sign of the left operand (notes §2.1).
    CHECK(print_of("7 / 2") == "(print 3)");
    CHECK(print_of("-7 / 2") == "(print -3)");
    CHECK(print_of("-7 % 3") == "(print -1)");
    CHECK(print_of("-2147483648 / -1") == "(print -2147483648)");
    CHECK(print_of("-2147483648 % -1") == "(print 0)");
}

TEST_CASE("fold: floats and mixed arithmetic") {
    CHECK(print_of("1 / 2.0") == "(print 0.5)");
    CHECK(print_of("1.5 + 1") == "(print 2.5)");
    CHECK(print_of("0.1 + 0.2") == "(print 0.30000000000000004)");
    CHECK(print_of("2 * 3.0") == "(print 6.0)");
    // IEEE results are values, not errors (notes §2.1).
    CHECK(print_of("1 / 0.0") == "(print inf)");
    CHECK(print_of("-1 / 0.0") == "(print -inf)");
    CHECK(print_of("0.0 / 0.0") == "(print nan)");
    // The sign of zero survives: -0.0 and 0.0 are different constants.
    CHECK(print_of("-0.0") == "(print -0.0)");
    CHECK(print_of("0.0 * -1") == "(print -0.0)");
}

TEST_CASE("fold: comparisons, equality and not") {
    CHECK(print_of("1 < 2") == "(print true)");
    CHECK(print_of("2 <= 1") == "(print false)");
    CHECK(print_of("1.5 > 1") == "(print true)");
    CHECK(print_of("1 >= 2.0") == "(print false)");
    CHECK(print_of("1 == 1.0") == "(print true)");
    CHECK(print_of("1 != 1.0") == "(print false)");
    CHECK(print_of("nil == nil") == "(print true)");
    CHECK(print_of("nil == false") == "(print false)");
    CHECK(print_of("true != false") == "(print true)");
    CHECK(print_of("0.0 / 0.0 == 0.0 / 0.0") == "(print false)");  // nan != nan
    CHECK(print_of("!true") == "(print false)");
    CHECK(print_of("!nil") == "(print true)");
    CHECK(print_of("!0") == "(print false)");  // 0 is truthy (notes §2.2)
    CHECK(print_of("!\"\"") == "(print false)");
    CHECK(print_of("!!1") == "(print true)");
}

TEST_CASE("fold: an operation that would fail at run time is left for run time") {
    const char* failing[] = {
        "1 / 0",        "1 % 0",     "-2147483648 / 0", "1 % 2.0",  "1.5 % 2", "1 + true",
        "1 + nil",      "true < 1",  "nil < nil",       "1 - \"a\"", "-true",   "-nil",
        "\"a\" - \"b\"", "-\"a\"",   "\"a\" + 1",       "1 < \"a\"",
    };
    for (const char* expr : failing) {
        INFO(expr);
        CHECK(folded(std::string("print ") + expr + ";") ==
              unfolded(std::string("print ") + expr + ";"));
    }
    // Only the failing operation stays; what can be folded around it is.
    CHECK(print_of("(1 / 0) + (2 + 3)") == "(print (+ (/ 1 0) 5))");
}

TEST_CASE("fold: strings are not folded, except as the test of a `!`, `and` or `or`") {
    CHECK(print_of("\"a\" + \"b\"") == "(print (+ \"a\" \"b\"))");
    CHECK(print_of("\"a\" == \"a\"") == "(print (== \"a\" \"a\"))");
    CHECK(print_of("\"a\" and 1") == "(print 1)");
    CHECK(print_of("\"\" or 1") == "(print \"\")");
}

TEST_CASE("fold: and / or return the deciding operand") {
    CHECK(print_of("true and x") == "(print x@global)");
    CHECK(print_of("false and x") == "(print false)");
    CHECK(print_of("nil and x") == "(print nil)");
    CHECK(print_of("0 and x") == "(print x@global)");
    CHECK(print_of("nil or x") == "(print x@global)");
    CHECK(print_of("false or x") == "(print x@global)");
    CHECK(print_of("1 or x") == "(print 1)");
    CHECK(print_of("1 and 2") == "(print 2)");
    CHECK(print_of("nil or false") == "(print false)");
    // The right operand is not folded away just because it is a literal: the left side decides.
    CHECK(print_of("x and true") == "(print (and x@global true))");
    CHECK(print_of("x or 1") == "(print (or x@global 1))");
    // A side-effecting right operand that is never evaluated disappears with the operator.
    CHECK(print_of("false and f()") == "(print false)");
    CHECK(print_of("true or f()") == "(print true)");
    // ...and one that is evaluated stays, folded inside.
    CHECK(print_of("true and f(1 + 1)") == "(print (call f@global 2))");
}

TEST_CASE("fold: folds inside every kind of expression") {
    CHECK(print_of("x + (1 + 2)") == "(print (+ x@global 3))");
    CHECK(print_of("(1 + 2) * x") == "(print (* 3 x@global))");
    CHECK(print_of("f(1 + 1, 2 * 3)") == "(print (call f@global 2 6))");
    CHECK(print_of("[1 + 1, 2 + 2]") == "(print (array 2 4))");
    CHECK(print_of("a[1 + 1]") == "(print (index a@global 2))");
    CHECK(folded("a[1 + 1] = 2 + 2;") == "(expr (set-index a@global 2 4))");
    CHECK(folded("x = 1 + 1;") == "(expr (set x@global 2))");
    CHECK(folded("let x = 1 + 1;") == "(let x 2)");
    CHECK(folded("fn f() { return 1 + 1; }") == "(fn f () (block (return 2)))");
    CHECK(print_of("-(1 + 1)") == "(print -2)");
    // Not a constant: left as it was.
    CHECK(print_of("x + y") == "(print (+ x@global y@global))");
}

TEST_CASE("fold: stats count what changed") {
    rung::FoldStats stats;
    folded("print 1 + 2; print !true; print x;", &stats);
    CHECK(stats.expressions_folded == 2);
    CHECK(stats.statements_removed == 0);
    folded("fn f() { return 1; print 1; print 2; }", &stats);
    CHECK(stats.expressions_folded == 0);
    CHECK(stats.statements_removed == 2);
}

TEST_CASE("fold: statements after a return in the same block are dropped") {
    CHECK(folded("fn f() { return 1; print 2; let x = 3; }") == "(fn f () (block (return 1)))");
    CHECK(folded("fn f() { print 1; return; print 2; }") ==
          "(fn f () (block (print 1) (return)))");
    // The dead code can hold anything, including functions and loops.
    CHECK(folded("fn f() { return 1; fn g() { return 2; } while (true) {} }") ==
          "(fn f () (block (return 1)))");
    // A return inside a nested block ends that block only.
    CHECK(folded("fn f(c) { { return 1; print 2; } print 3; }") ==
          "(fn f (c) (block (block (return 1)) (print 3)))");
    // A return that may not be taken ends nothing.
    CHECK(folded("fn f(c) { if (c) return 1; print 2; }") ==
          "(fn f (c) (block (if c@0 (return 1)) (print 2)))");
}

TEST_CASE("fold: an if with a literal condition keeps only the branch that runs") {
    CHECK(folded("if (true) print 1; else print 2;") == "(print 1)");
    CHECK(folded("if (false) print 1; else print 2;") == "(print 2)");
    CHECK(folded("if (false) print 1;") == "");
    CHECK(folded("if (nil) print 1; print 2;") == "(print 2)");
    CHECK(folded("if (0) print 1; else print 2;") == "(print 1)");  // 0 is truthy
    CHECK(folded("if (1 < 2) { print 1; } else { print 2; }") == "(block (print 1))");
    // The condition is folded first.
    CHECK(folded("if (1 > 2 or 2 > 3) print 1; else print 2;") == "(print 2)");
    // Not a literal: both branches stay, folded inside.
    CHECK(folded("if (x) print 1 + 1; else print 2 + 2;") ==
          "(if x@global (print 2) (print 4))");
    // As the body of another statement a vanished `if` is an empty block, since a body is
    // always a statement.
    CHECK(folded("if (x) if (false) print 1;") == "(if x@global (block))");
    CHECK(folded("while (x) if (false) print 1;") == "(while x@global (block))");
}

TEST_CASE("fold: a return that an if makes certain ends its block") {
    CHECK(folded("fn f() { if (true) return 1; print 2; }") == "(fn f () (block (return 1)))");
    CHECK(folded("fn f() { if (false) return 1; print 2; }") == "(fn f () (block (print 2)))");
}

TEST_CASE("fold: a while with a falsy literal condition is dropped") {
    CHECK(folded("while (false) print 1; print 2;") == "(print 2)");
    CHECK(folded("while (nil) { print 1; } print 2;") == "(print 2)");
    CHECK(folded("while (1 > 2) print 1; print 2;") == "(print 2)");
    // A truthy literal is an infinite loop, which must stay.
    CHECK(folded("while (true) { print 1; }") == "(while true (block (print 1)))");
    CHECK(folded("while (1) { print 1; }") == "(while 1 (block (print 1)))");
    // A `for` keeps its initializer even if the loop never runs.
    CHECK(folded("for (let i = f(); false; i = i + 1) print i;") ==
          "(block (let i (call f@global)))");
}

TEST_CASE("fold: removing code never changes a variable's scope distance") {
    // The kept `if` branch is a block, so it stays one scope; `a` is still one scope up.
    CHECK(folded("{ let a = 1; if (true) { print a; } else { print 2; } }") ==
          "(block (let a 1) (block (print a@1)))");
    CHECK(folded("{ let a = 1; fn g() { return a; print 9; } }") ==
          "(block (let a 1) (fn g () (block (return a@1))))");
    CHECK(folded("{ let a = 1; while (false) { print a; } print a; }") ==
          "(block (let a 1) (print a@0))");
}

TEST_CASE("fold: leaves a program with nothing to fold exactly as it was") {
    const char* source =
        "fn fib(n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }\n"
        "let i = 0; while (i < 3) { print fib(i); i = i + 1; }\n";
    CHECK(folded(source) == unfolded(source));
}

TEST_CASE("fold: an expression chain at the 1000-link limit folds and is destroyed safely") {
    std::string chain = "print 1";
    for (int i = 0; i < 999; ++i) chain += " + 1";
    chain += ";";
    CHECK(folded(chain) == "(print 1000)");

    std::string with_variable = "print x";
    for (int i = 0; i < 999; ++i) with_variable += " + 1";
    with_variable += ";";
    CHECK(rung::dump_ast(*resolved(with_variable)).size() > 0);
    rung::FoldStats stats;
    folded(with_variable, &stats);
    CHECK(stats.expressions_folded == 0);  // ((x + 1) + 1) + ...: each + has a variable below it

    // A dropped operand that is itself a long chain.
    std::string dropped = "print false and 1";
    for (int i = 0; i < 998; ++i) dropped += " + 1";
    dropped += ";";
    CHECK(folded(dropped) == "(print false)");
}

// ---- Behaviour: folding changes nothing a program can observe -------------------------------

TEST_CASE("fold: every engine gives the same output and error with and without folding") {
    const char* programs[] = {
        "print 2147483647 + 1; print -2147483648 - 1; print 65536 * 65536;",
        "print 7 / 2; print -7 / 2; print -7 % 3; print -2147483648 / -1;",
        "print 1 / 2.0; print 0.1 + 0.2; print 1 / 0.0; print -1 / 0.0; print 0.0 / 0.0;",
        "print -0.0; print 0.0 * -1; print 0.0; print 0.0 - 0.0;",
        "print 1 == 1.0; print nil == false; print !0; print !nil; print !\"\";",
        "print true and 5; print false and 5; print nil or \"x\"; print 0 or 1; print 1 and nil;",
        "fn f() { print \"f\"; return 1; } print false and f(); print true or f();"
        " print true and f();",
        "print 1; print 1 / 0; print 2;",
        "print 1;\nprint 2;\nprint 1 % 0;\nprint 3;",
        "print 1;\nprint\n1\n+\ntrue;\n",
        "print 1 + 1;\nprint -\"a\";",
        "print 1 < 2;\nprint 1 < nil;",
        "print 1 % 2.0;",
        "print \"a\" + \"b\"; print \"a\" == \"a\"; print \"a\" + 1;",
        "fn f() { return 1; print 2; } print f();",
        "fn f(c) { if (c) return 1; print \"after\"; return 2; } print f(true); print f(false);",
        "if (false) print 1; else print 2; if (true) print 3; else print 4;",
        "while (false) print 1; print 5;",
        "let n = 0; while (true) { n = n + 1; if (n > 3) { print n; return_it(); } }"
        " fn return_it() {}",
        "for (let i = 0; i < 3; i = i + 1) { if (false) print 1; print i; }",
        "for (let i = 0; false; i = i + 1) print i; print \"done\";",
        "{ let a = 1; if (true) { print a; } else { print 2; } }",
        "let g = 5; { let g = 6; fn h() { return g; return 0; } print h(); } print g;",
        "fn make() { let c = 0; fn inc() { c = c + 1; return c; print 0; } return inc; }"
        " let f = make(); print f(); print f();",
        "let a = [1 + 1, 2 * 3]; a[1 + 0] = 9 - 1; print a; print a[0 + 0];",
        "print len([1, 2, 3]) + 1 + 2;",
        "print -2147483648; print - -2147483647; print !!nil;",
    };
    for (const char* program : programs) {
        INFO(program);
        for (const char* engine : {"tree", "stack", "register"}) {
            INFO(engine);
            Run plain = run_on(engine, program, false);
            Run fold = run_on(engine, program, true);
            CHECK(fold.output == plain.output);
            CHECK(fold.error == plain.error);
        }
    }
}

TEST_CASE("fold: a runtime error keeps its line and happens after earlier output") {
    Run run = run_on("register", "print 1;\nprint 2;\nprint 1 / 0;\nprint 3;", true);
    CHECK(run.output == "1\n2\n");
    CHECK(run.error == "[line 3] runtime error: division by zero");

    // The error is at the operator's line, not the statement's.
    Run split = run_on("stack", "print 1 +\n  2 +\n  true;", true);
    CHECK(split.error == "[line 2] runtime error: operands must be two numbers or two strings");
}

TEST_CASE("fold: the folded program compiles to less bytecode") {
    const std::string source = "print 1 + 2 * 3; print 2147483647 + 1;";
    auto size_of = [&](bool fold, bool reg) {
        std::unique_ptr<rung::Program> program = resolved(source);
        if (fold) rung::fold(*program);
        rung::Heap heap;
        if (reg) {
            rung::RegCompileResult compiled = rung::compile_register(*program, heap);
            REQUIRE(compiled.ok());
            return rung::register_bytecode_size(*compiled.function).instructions;
        }
        rung::StackCompileResult compiled = rung::compile_stack(*program, heap);
        REQUIRE(compiled.ok());
        return rung::stack_bytecode_size(*compiled.function).instructions;
    };
    // Register: PRINT of a constant twice, then RETURN_NIL. Stack: CONST PRINT twice, then
    // NIL RETURN. Unfolded, each of the two expressions takes its own arithmetic.
    CHECK(size_of(true, true) < size_of(false, true));
    CHECK(size_of(true, false) < size_of(false, false));
    CHECK(size_of(true, true) == 3);
}
