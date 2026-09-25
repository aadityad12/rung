#include <doctest.h>

#include <string>

#include "ast_dump.h"
#include "parser.h"
#include "resolver.h"

namespace {

// Parses, resolves, and returns the --dump-ast text (trailing newline removed) with binding
// annotations, or the formatted error. A parse error would be a bug in a test's source.
std::string bound(const std::string& source) {
    rung::ParseResult parsed = rung::parse(source);
    if (!parsed.ok()) return "PARSE ERROR: " + rung::format_error(*parsed.error);
    if (auto error = rung::resolve(*parsed.program)) return rung::format_error(*error);
    std::string out = rung::dump_ast(*parsed.program);
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// "[line N] compile error: msg" or "no error".
std::string error_of(const std::string& source) {
    rung::ParseResult parsed = rung::parse(source);
    if (!parsed.ok()) return "PARSE ERROR: " + rung::format_error(*parsed.error);
    auto error = rung::resolve(*parsed.program);
    return error ? rung::format_error(*error) : "no error";
}

// "let a0; let a1; ... " with `n` names.
std::string lets(const std::string& prefix, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += "let " + prefix + std::to_string(i) + "; ";
    return out;
}

// "a0; a1; ..." : expression statements that read `n` names.
std::string uses(const std::string& prefix, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += prefix + std::to_string(i) + "; ";
    return out;
}

// "p0, p1, ..." with `n` names, joined by `sep`.
std::string names(const std::string& prefix, int n, const std::string& sep = ", ") {
    std::string out;
    for (int i = 0; i < n; ++i) {
        if (i > 0) out += sep;
        out += prefix + std::to_string(i);
    }
    return out;
}

std::string call_with(int n) {
    std::string out = "f(";
    for (int i = 0; i < n; ++i) out += i == 0 ? "nil" : ", nil";
    return out + ")";
}

}  // namespace

TEST_CASE("resolver: globals and top-level code bind as global") {
    CHECK(bound("a = 1; print a;") == "(expr (set a@global 1))\n(print a@global)");
    CHECK(bound("let a = 1; let a = a + 1;") == "(let a 1)\n(let a (+ a@global 1))");
    CHECK(bound("fn f() { print g; }") == "(fn f () (block (print g@global)))");
    CHECK(error_of("let a = 1; let a = 2;") == "no error");  // globals may be redeclared
    CHECK(error_of("let a = a;") == "no error");              // a global read, not an error
}

TEST_CASE("resolver: the D11 example keeps binding to the global") {
    // The closure must print "global" both times: `a` inside show() was resolved when the
    // function was declared, before the block-local `a` existed.
    CHECK(bound("let a = \"global\";\n"
                "{ fn show() { print a; } show(); let a = \"block\"; show(); }") ==
          "(let a \"global\")\n"
          "(block (fn show () (block (print a@global))) (expr (call show@0)) "
          "(let a \"block\") (expr (call show@0)))");
    // Had the local come first, the same use binds to it, one scope (the function's) up.
    CHECK(bound("{ let a = 1; fn show() { print a; } }") ==
          "(block (let a 1) (fn show () (block (print a@1))))");
}

TEST_CASE("resolver: shadowing in nested blocks") {
    CHECK(bound("{ let a = 1; { let a = 2; print a; } print a; }") ==
          "(block (let a 1) (block (let a 2) (print a@0)) (print a@0))");
    CHECK(bound("{ let a = 1; { print a; } }") == "(block (let a 1) (block (print a@1)))");
    CHECK(bound("{ let a = 1; { { print a; } } }") ==
          "(block (let a 1) (block (block (print a@2))))");
    CHECK(bound("{ let a; a = 3; }") == "(block (let a) (expr (set a@0 3)))");
    // The inner `a` is declared (but not defined) while its initializer runs, so it hides the
    // outer one and the read is an error rather than a read of the outer `a`.
    CHECK(error_of("{ let a = 1; { let a = a + 1; print a; } }") ==
          "[line 1] compile error: can't read local variable 'a' in its own initializer");
    // Sibling blocks do not see each other's locals.
    CHECK(bound("{ let a; } print a;") == "(block (let a))\n(print a@global)");
    CHECK(bound("if (true) { let a = 1; print a; } else { print a; }") ==
          "(if true (block (let a 1) (print a@0)) (block (print a@global)))");
    CHECK(bound("while (true) { let a = 1; print a; }") ==
          "(while true (block (let a 1) (print a@0)))");
}

TEST_CASE("resolver: parameters live in the function's scope") {
    CHECK(bound("let a = 1; fn f(a) { print a; }") ==
          "(let a 1)\n(fn f (a) (block (print a@0)))");
    CHECK(bound("fn f(a, b) { print a + b; }") ==
          "(fn f (a b) (block (print (+ a@0 b@0))))");
    // The body's own statements share the parameter scope: no extra hop for the body block...
    CHECK(bound("fn f(a) { let b = a; }") == "(fn f (a) (block (let b a@0)))");
    // ...but a nested block does add one.
    CHECK(bound("fn f(a) { { print a; } }") == "(fn f (a) (block (block (print a@1))))");
    // A parameter and a body local are in the same scope, so they clash.
    CHECK(error_of("fn f(a) { let a; }") ==
          "[line 1] compile error: variable 'a' is already declared in this scope");
    CHECK(error_of("fn f(a) { { let a; } }") == "no error");
    CHECK(error_of("fn f(a, a) {}") ==
          "[line 1] compile error: variable 'a' is already declared in this scope");
}

TEST_CASE("resolver: functions can recurse") {
    CHECK(bound("fn f(n) { f(n); }") == "(fn f (n) (block (expr (call f@global n@0))))");
    // A local function is defined in its enclosing scope before its body is resolved, so the
    // body reaches it one scope up (the function's own scope is in between).
    CHECK(bound("{ fn f(n) { f(n); } }") == "(block (fn f (n) (block (expr (call f@1 n@0)))))");
    CHECK(bound("{ fn even(n) { odd(n); } fn odd(n) { even(n); } }") ==
          "(block (fn even (n) (block (expr (call odd@global n@0)))) "
          "(fn odd (n) (block (expr (call even@1 n@0)))))");
}

TEST_CASE("resolver: hops through nested blocks and nested functions") {
    CHECK(bound("fn outer() { let x = 1; fn inner() { { print x; } } }") ==
          "(fn outer () (block (let x 1) (fn inner () (block (block (print x@2))))))");
    CHECK(bound("fn a() { let x; fn b() { let y; fn c() { print x + y; } } }") ==
          "(fn a () (block (let x) (fn b () (block (let y) (fn c () "
          "(block (print (+ x@2 y@1))))))))");
    CHECK(bound("{ let x; { fn f() { { { print x; } } } } }") ==
          "(block (let x) (block (fn f () (block (block (block (print x@4)))))))");
    // Innermost declaration wins across function boundaries.
    CHECK(bound("{ let x; fn f() { let x; print x; } }") ==
          "(block (let x) (fn f () (block (let x) (print x@0))))");
}

TEST_CASE("resolver: for loops bind through their desugared blocks") {
    // The loop variable lives in the outer block; the body and increment are one scope in.
    CHECK(bound("for (let i = 0; i < 3; i = i + 1) print i;") ==
          "(block (let i 0) (while (< i@0 3) (block (print i@1) (expr (set i@1 (+ i@1 1))))))");
    CHECK(bound("for (i = 0; ; ) print i;") ==
          "(block (expr (set i@global 0)) (while true (block (print i@global))))");
}

TEST_CASE("resolver: return outside a function") {
    CHECK(error_of("return 1;") == "[line 1] compile error: can't return from top-level code");
    CHECK(error_of("return;") == "[line 1] compile error: can't return from top-level code");
    CHECK(error_of("\n{ if (true) return 1; }") ==
          "[line 2] compile error: can't return from top-level code");
    CHECK(error_of("fn f() { return 1; } return 2;") ==
          "[line 1] compile error: can't return from top-level code");
    CHECK(error_of("fn f() { return 1; } fn g() { { return; } }") == "no error");
}

TEST_CASE("resolver: a local cannot be read in its own initializer") {
    CHECK(error_of("{ let a = a; }") ==
          "[line 1] compile error: can't read local variable 'a' in its own initializer");
    CHECK(error_of("{ let a = 1; { let a = a; } }") ==
          "[line 1] compile error: can't read local variable 'a' in its own initializer");
    CHECK(error_of("fn f() {\n let a = 1 + a;\n}") ==
          "[line 2] compile error: can't read local variable 'a' in its own initializer");
    // Assigning to it there is just as broken (no slot exists yet), so it is the same error.
    CHECK(error_of("{ let a = (a = 1); }") ==
          "[line 1] compile error: can't read local variable 'a' in its own initializer");
    // A different name, or the initializer of a global, is fine.
    CHECK(error_of("{ let a = 1; let b = a; }") == "no error");
}

TEST_CASE("resolver: redeclaring a local in the same scope") {
    CHECK(error_of("{ let x; let x; }") ==
          "[line 1] compile error: variable 'x' is already declared in this scope");
    CHECK(error_of("{\n let x;\n let x;\n}") ==
          "[line 3] compile error: variable 'x' is already declared in this scope");
    CHECK(error_of("{ let f; fn f() {} }") ==
          "[line 1] compile error: variable 'f' is already declared in this scope");
    CHECK(error_of("{ fn f() {} fn f() {} }") ==
          "[line 1] compile error: variable 'f' is already declared in this scope");
    CHECK(error_of("{ let x; { let x; } }") == "no error");
    CHECK(error_of("{ let x; } { let x; }") == "no error");
    CHECK(error_of("fn f() { let x; } fn g() { let x; }") == "no error");
}

TEST_CASE("resolver: the name's own line is reported") {
    // The error is at the name token, even when `let` is on an earlier line.
    CHECK(error_of("{ let x;\n let\n x; }") ==
          "[line 3] compile error: variable 'x' is already declared in this scope");
}

TEST_CASE("resolver: at most 255 parameters") {
    CHECK(error_of("fn f(" + names("p", 255) + ") {}") == "no error");
    CHECK(error_of("fn f(" + names("p", 256) + ") {}") ==
          "[line 1] compile error: too many parameters");
    // The line is the 256th parameter's.
    CHECK(error_of("fn f(" + names("p", 256, ",\n") + ") {}") ==
          "[line 256] compile error: too many parameters");
}

TEST_CASE("resolver: at most 255 call arguments") {
    CHECK(error_of(call_with(255) + ";") == "no error");
    CHECK(error_of(call_with(256) + ";") == "[line 1] compile error: too many arguments");
    CHECK(error_of("\n" + call_with(300) + ";") == "[line 2] compile error: too many arguments");
    CHECK(error_of("f(" + call_with(256) + ");") == "[line 1] compile error: too many arguments");
}

TEST_CASE("resolver: at most 255 locals live in one function") {
    CHECK(error_of("{ " + lets("v", 255) + "}") == "no error");
    CHECK(error_of("{ " + lets("v", 256) + "}") ==
          "[line 1] compile error: too many local variables");
    CHECK(error_of("fn f() { " + lets("v", 255) + "}") == "no error");
    CHECK(error_of("fn f() { " + lets("v", 256) + "}") ==
          "[line 1] compile error: too many local variables");
    // Parameters are locals too.
    CHECK(error_of("fn f(" + names("p", 254) + ") { let x; }") == "no error");
    CHECK(error_of("fn f(" + names("p", 255) + ") { let x; }") ==
          "[line 1] compile error: too many local variables");
    // "Live at once": leaving a block frees its locals, and blocks add up while nested.
    CHECK(error_of("{ " + lets("a", 255) + "} { " + lets("b", 255) + "}") == "no error");
    CHECK(error_of("{ " + lets("a", 100) + "{ " + lets("b", 100) + "} " + lets("c", 100) + "}") ==
          "no error");
    CHECK(error_of("{ " + lets("a", 200) + "{ " + lets("b", 55) + "} }") == "no error");
    CHECK(error_of("{ " + lets("a", 200) + "{ " + lets("b", 56) + "} }") ==
          "[line 1] compile error: too many local variables");
    // The count is per function: a nested function starts from zero. Its name is a local of
    // the outer function, so the outer one has room for 254 other locals.
    CHECK(error_of("fn f() { " + lets("a", 254) + "fn g() { " + lets("b", 255) + "} }") ==
          "no error");
    CHECK(error_of("fn f() { " + lets("a", 255) + "fn g() {} }") ==
          "[line 1] compile error: too many local variables");
    // Top-level `let`s are globals, which have no limit; the script's blocks do.
    CHECK(error_of(lets("g", 300)) == "no error");
    // The line is the offending name's.
    CHECK(error_of("{ " + lets("v", 255) + "\nlet last; }") ==
          "[line 2] compile error: too many local variables");
}

TEST_CASE("resolver: at most 255 captured variables per function") {
    // Enclosing block: 200 locals; f1 has 56 of its own. `inner` (in f1) reads them all.
    auto direct = [](bool overflow) {
        return "{ " + lets("a", 200) + "fn f1() { " + lets("b", 56) + "fn inner() {\n" +
               uses("a", 200) + uses("b", overflow ? 56 : 55) + "\n} } }";
    };
    CHECK(error_of(direct(false)) == "no error");
    CHECK(error_of(direct(true)) == "[line 2] compile error: too many captured variables");

    // A variable is counted once however often it is used.
    {
        std::string body;
        for (int i = 0; i < 300; ++i) body += "x; ";
        CHECK(error_of("{ let x; fn f() { " + body + "} }") == "no error");
    }
    // Globals are not captured, however many a function reads.
    {
        std::string body;
        for (int i = 0; i < 300; ++i) body += "g" + std::to_string(i) + "; ";
        CHECK(error_of("fn f() { " + body + "}") == "no error");
    }
    // Locals of the function itself are not captures.
    CHECK(error_of("fn f() { " + lets("v", 255) + uses("v", 255) + "}") == "no error");
}

TEST_CASE("resolver: captures are counted through intermediate functions") {
    // `mid` reads 255 outer variables itself. `inner` reads one more outer variable (b55) and
    // nothing else; the value has to pass through `mid`, so `mid` now captures 256.
    auto through = [](bool overflow) {
        return "{ " + lets("a", 200) + "fn f1() { " + lets("b", 56) + "fn mid() { " +
               uses("a", 200) + uses("b", 55) + "fn inner() {\n" + (overflow ? "b55;" : "b54;") +
               " } } } }";
    };
    CHECK(error_of(through(false)) == "no error");
    CHECK(error_of(through(true)) == "[line 2] compile error: too many captured variables");
    // Same program flattened: reading them all from `inner` directly is over the limit too.
    CHECK(error_of("{ " + lets("a", 200) + "fn f1() { " + lets("b", 56) + "fn inner() { " +
                   uses("a", 200) + uses("b", 56) + "} } }") ==
          "[line 1] compile error: too many captured variables");
}

TEST_CASE("resolver: the deepest legal nesting resolves") {
    std::string open, close;
    for (int i = 0; i < 100; ++i) {
        open += "{ ";
        close += "} ";
    }
    CHECK(error_of("{ let x; " + open + "print x; " + close + "}") == "no error");
    CHECK(bound("{ let x; " + open + "print x; " + close + "}").find("x@100") != std::string::npos);
}

TEST_CASE("resolver: stops at the first error") {
    CHECK(error_of("{ let a; let a; }\nreturn 1;") ==
          "[line 1] compile error: variable 'a' is already declared in this scope");
    CHECK(error_of("return 1;\n{ let a; let a; }") ==
          "[line 1] compile error: can't return from top-level code");
}

TEST_CASE("resolver: unresolved trees dump without annotations") {
    // The parser tests rely on this: a tree that has not been resolved prints bare names.
    rung::ParseResult parsed = rung::parse("print a; a = 1;");
    REQUIRE(parsed.ok());
    CHECK(rung::dump_ast(*parsed.program) == "(print a)\n(expr (set a 1))\n");
}
