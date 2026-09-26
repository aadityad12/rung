#include <doctest.h>

#include <string>

#include "ast_dump.h"
#include "parser.h"

using rung::parse;

namespace {

// The --dump-ast text of a program, with the trailing newline removed so single statements
// compare naturally. Multi-statement programs keep their inner newlines.
std::string dump(const std::string& source) {
    rung::ParseResult result = parse(source);
    if (!result.ok()) return "ERROR: " + rung::format_error(*result.error);
    std::string out = rung::dump_ast(*result.program);
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// "[line N] compile error: msg" or "no error".
std::string error_of(const std::string& source) {
    rung::ParseResult result = parse(source);
    return result.ok() ? "no error" : rung::format_error(*result.error);
}

std::string repeat(const std::string& text, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += text;
    return out;
}

}  // namespace

TEST_CASE("empty program") { CHECK(dump("") == ""); }

TEST_CASE("literals") {
    CHECK(dump("1;") == "(expr 1)");
    CHECK(dump("2.5;") == "(expr 2.5)");
    CHECK(dump("3.0;") == "(expr 3.0)");
    CHECK(dump("\"hi\\n\";") == "(expr \"hi\\n\")");
    CHECK(dump("true; false; nil;") == "(expr true)\n(expr false)\n(expr nil)");
    CHECK(dump("x;") == "(expr x)");
}

TEST_CASE("precedence at every level") {
    CHECK(dump("print 1 + 2 * 3;") == "(print (+ 1 (* 2 3)))");
    CHECK(dump("print 1 * 2 + 3;") == "(print (+ (* 1 2) 3))");
    CHECK(dump("print 1 + 2 * 3 - 4;") == "(print (- (+ 1 (* 2 3)) 4))");
    CHECK(dump("print 1 < 2 + 3;") == "(print (< 1 (+ 2 3)))");
    CHECK(dump("print 1 == 2 < 3;") == "(print (== 1 (< 2 3)))");
    CHECK(dump("print a == b and c;") == "(print (and (== a b) c))");
    CHECK(dump("print a or b and c;") == "(print (or a (and b c)))");
    CHECK(dump("print a and b or c;") == "(print (or (and a b) c))");
    CHECK(dump("print -a * b;") == "(print (* (- a) b))");
    CHECK(dump("print !a == b;") == "(print (== (! a) b))");
    CHECK(dump("print -f(1);") == "(print (- (call f 1)))");
    CHECK(dump("print (1 + 2) * 3;") == "(print (* (+ 1 2) 3))");
    CHECK(dump("print 7 % 3 * 2;") == "(print (* (% 7 3) 2))");
    CHECK(dump("print a != b >= c <= d > e;") == "(print (!= a (> (<= (>= b c) d) e)))");
}

TEST_CASE("binary operators are left-associative") {
    CHECK(dump("print 1 - 2 - 3;") == "(print (- (- 1 2) 3))");
    CHECK(dump("print 8 / 4 / 2;") == "(print (/ (/ 8 4) 2))");
    CHECK(dump("print a == b == c;") == "(print (== (== a b) c))");
    CHECK(dump("print a < b < c;") == "(print (< (< a b) c))");
    CHECK(dump("print a or b or c;") == "(print (or (or a b) c))");
    CHECK(dump("print a and b and c;") == "(print (and (and a b) c))");
}

TEST_CASE("assignment is right-associative") {
    CHECK(dump("a = 1;") == "(expr (set a 1))");
    CHECK(dump("a = b = c;") == "(expr (set a (set b c)))");
    CHECK(dump("a = b or c;") == "(expr (set a (or b c)))");
    CHECK(dump("a[0] = 1;") == "(expr (set-index a 0 1))");
    CHECK(dump("a[0] = b[1] = 2;") == "(expr (set-index a 0 (set-index b 1 2)))");
    CHECK(dump("a[i][j] = 3;") == "(expr (set-index (index a i) j 3))");
    CHECK(dump("f()[0] = 3;") == "(expr (set-index (call f) 0 3))");
    CHECK(dump("(a) = 1;") == "(expr (set a 1))");
}

TEST_CASE("unary chains") {
    CHECK(dump("print --x;") == "(print (- (- x)))");
    CHECK(dump("print !!x;") == "(print (! (! x)))");
    CHECK(dump("print -!x;") == "(print (- (! x)))");
    CHECK(dump("print - -x;") == "(print (- (- x)))");
}

TEST_CASE("-2147483648 is the only place 2147483648 is legal") {
    CHECK(dump("print -2147483648;") == "(print -2147483648)");
    CHECK(dump("print - 2147483648;") == "(print -2147483648)");
    CHECK(dump("print -2147483647;") == "(print (- 2147483647))");
    CHECK(dump("print 2147483647;") == "(print 2147483647)");
    CHECK(dump("print -2147483648 + 1;") == "(print (+ -2147483648 1))");
    CHECK(dump("print --2147483648;") == "(print (- -2147483648))");
    CHECK(error_of("print 2147483648;") ==
          "[line 1] compile error: integer literal '2147483648' is too large for a 32-bit int");
    CHECK(error_of("print -(2147483648);") ==
          "[line 1] compile error: integer literal '2147483648' is too large for a 32-bit int");
    CHECK(error_of("print !2147483648;") ==
          "[line 1] compile error: integer literal '2147483648' is too large for a 32-bit int");
    // A postfix binds tighter than unary minus, so this is -(2147483648[0]).
    CHECK(error_of("print -2147483648[0];") ==
          "[line 1] compile error: integer literal '2147483648' is too large for a 32-bit int");
    // Larger values are the lexer's error.
    CHECK(error_of("print 2147483649;") ==
          "[line 1] compile error: integer literal '2147483649' is too large for a 32-bit int");
}

TEST_CASE("postfix chains") {
    CHECK(dump("f(1)(2)[0];") == "(expr (index (call (call f 1) 2) 0))");
    CHECK(dump("f();") == "(expr (call f))");
    CHECK(dump("f(1, 2, 3);") == "(expr (call f 1 2 3))");
    CHECK(dump("a[0][1];") == "(expr (index (index a 0) 1))");
    CHECK(dump("a[f(1)];") == "(expr (index a (call f 1)))");
    CHECK(dump("[1, 2][0];") == "(expr (index (array 1 2) 0))");
}

TEST_CASE("array literals") {
    CHECK(dump("[];") == "(expr (array))");
    CHECK(dump("[1];") == "(expr (array 1))");
    CHECK(dump("[1, 2 + 3, x];") == "(expr (array 1 (+ 2 3) x))");
    CHECK(dump("[[], [1, [2]]];") == "(expr (array (array) (array 1 (array 2))))");
}

TEST_CASE("declarations and simple statements") {
    CHECK(dump("let x = 5;") == "(let x 5)");
    CHECK(dump("let x;") == "(let x)");
    CHECK(dump("print 1;") == "(print 1)");
    CHECK(dump("return;") == "(return)");
    CHECK(dump("return 1 + 2;") == "(return (+ 1 2))");
    CHECK(dump("{}") == "(block)");
    CHECK(dump("{ let a = 1; print a; }") == "(block (let a 1) (print a))");
    CHECK(dump("fn add(a, b) { return a + b; }") ==
          "(fn add (a b) (block (return (+ a b))))");
    CHECK(dump("fn nothing() {}") == "(fn nothing () (block))");
    CHECK(dump("fn one(a) { fn inner() {} }") ==
          "(fn one (a) (block (fn inner () (block))))");
}

TEST_CASE("if, else, and while") {
    CHECK(dump("if (a) print 1;") == "(if a (print 1))");
    CHECK(dump("if (a) print 1; else print 2;") == "(if a (print 1) (print 2))");
    CHECK(dump("while (a < 3) a = a + 1;") == "(while (< a 3) (expr (set a (+ a 1))))");
    CHECK(dump("while (true) { break_; }") == "(while true (block (expr break_)))");
}

TEST_CASE("else binds to the nearest if") {
    CHECK(dump("if (a) if (b) print 1; else print 2;") ==
          "(if a (if b (print 1) (print 2)))");
    CHECK(dump("if (a) { if (b) print 1; } else print 2;") ==
          "(if a (block (if b (print 1))) (print 2))");
    CHECK(dump("if (a) print 1; else if (b) print 2; else print 3;") ==
          "(if a (print 1) (if b (print 2) (print 3)))");
}

TEST_CASE("for is desugared into a block and a while") {
    CHECK(dump("for (let i = 0; i < 3; i = i + 1) print i;") ==
          "(block (let i 0) (while (< i 3) (block (print i) (expr (set i (+ i 1))))))");
    // Each omitted clause.
    CHECK(dump("for (; i < 3; i = i + 1) print i;") ==
          "(block (while (< i 3) (block (print i) (expr (set i (+ i 1))))))");
    CHECK(dump("for (let i = 0; ; i = i + 1) print i;") ==
          "(block (let i 0) (while true (block (print i) (expr (set i (+ i 1))))))");
    CHECK(dump("for (let i = 0; i < 3;) print i;") ==
          "(block (let i 0) (while (< i 3) (block (print i))))");
    CHECK(dump("for (;;) print 1;") == "(block (while true (block (print 1))))");
    // An expression initializer.
    CHECK(dump("for (i = 0; i < 3; i = i + 1) {}") ==
          "(block (expr (set i 0)) (while (< i 3) (block (block) (expr (set i (+ i 1))))))");
}

TEST_CASE("the AST carries line numbers") {
    rung::ParseResult result = parse("let a = 1;\nprint a\n  + 2;\nf(\n1);\na[0];");
    REQUIRE(result.ok());
    const auto& stmts = result.program->statements;
    REQUIRE(stmts.size() == 4);
    CHECK(std::get<rung::Let>(stmts[0]->node).line == 1);
    const auto& print = std::get<rung::Print>(stmts[1]->node);
    CHECK(print.line == 2);
    CHECK(std::get<rung::Binary>(print.value->node).line == 3);  // the '+'
    const auto& call = std::get<rung::Call>(std::get<rung::ExprStmt>(stmts[2]->node).expr->node);
    CHECK(call.line == 4);  // the '('
    const auto& index = std::get<rung::Index>(std::get<rung::ExprStmt>(stmts[3]->node).expr->node);
    CHECK(index.line == 6);  // the '['
}

TEST_CASE("resolver fields start unresolved") {
    rung::ParseResult result = parse("x; x = 1;");
    REQUIRE(result.ok());
    const auto& stmts = result.program->statements;
    const auto& var = std::get<rung::Variable>(std::get<rung::ExprStmt>(stmts[0]->node).expr->node);
    const auto& assign =
        std::get<rung::Assign>(std::get<rung::ExprStmt>(stmts[1]->node).expr->node);
    CHECK(var.binding.kind == rung::BindingKind::Unresolved);
    CHECK(assign.binding.kind == rung::BindingKind::Unresolved);
}

TEST_CASE("names stay valid after the ParseResult is moved") {
    rung::ParseResult result = parse("let a_long_variable_name_beyond_sso_length_xxxxxxxx = 1;");
    REQUIRE(result.ok());
    rung::ParseResult moved = std::move(result);
    CHECK(rung::dump_ast(*moved.program) ==
          "(let a_long_variable_name_beyond_sso_length_xxxxxxxx 1)\n");
    rung::ParseResult tiny = parse("let a = 1;");
    rung::ParseResult tiny_moved = std::move(tiny);
    CHECK(rung::dump_ast(*tiny_moved.program) == "(let a 1)\n");
}

TEST_CASE("parse errors and their lines") {
    CHECK(error_of("print 1") == "[line 1] compile error: expected ';' after value");
    CHECK(error_of("1 + 2") == "[line 1] compile error: expected ';' after expression");
    CHECK(error_of("let x = 1") ==
          "[line 1] compile error: expected ';' after variable declaration");
    CHECK(error_of("return 1") == "[line 1] compile error: expected ';' after return value");
    CHECK(error_of("print 1\n\n") == "[line 3] compile error: expected ';' after value");
    CHECK(error_of("print ;") == "[line 1] compile error: expected expression");
    CHECK(error_of("print 1 +;") == "[line 1] compile error: expected expression");
    CHECK(error_of("else print 1;") == "[line 1] compile error: expected expression");
    CHECK(error_of("(1 + 2;") == "[line 1] compile error: expected ')' after expression");
    CHECK(error_of("f(1, 2;") == "[line 1] compile error: expected ')' after arguments");
    CHECK(error_of("f(1,);") == "[line 1] compile error: expected expression");
    CHECK(error_of("a[1;") == "[line 1] compile error: expected ']' after index");
    CHECK(error_of("[1, 2;") == "[line 1] compile error: expected ']' after array elements");
    CHECK(error_of("[1,];") == "[line 1] compile error: expected expression");
    CHECK(error_of("let 5 = 1;") == "[line 1] compile error: expected variable name after 'let'");
    CHECK(error_of("fn 5() {}") == "[line 1] compile error: expected function name after 'fn'");
    CHECK(error_of("fn f {}") == "[line 1] compile error: expected '(' after function name");
    CHECK(error_of("fn f(1) {}") == "[line 1] compile error: expected parameter name");
    CHECK(error_of("fn f(a,) {}") == "[line 1] compile error: expected parameter name");
    CHECK(error_of("fn f(a b) {}") == "[line 1] compile error: expected ')' after parameters");
    CHECK(error_of("fn f() print 1;") ==
          "[line 1] compile error: expected '{' before function body");
    CHECK(error_of("{ print 1;") == "[line 1] compile error: expected '}' after block");
    CHECK(error_of("{\nprint 1;\n") == "[line 3] compile error: expected '}' after block");
    CHECK(error_of("if x) print 1;") == "[line 1] compile error: expected '(' after 'if'");
    CHECK(error_of("if (x print 1;") == "[line 1] compile error: expected ')' after condition");
    CHECK(error_of("while x) print 1;") == "[line 1] compile error: expected '(' after 'while'");
    CHECK(error_of("while (x print 1;") == "[line 1] compile error: expected ')' after condition");
    CHECK(error_of("for i;;) {}") == "[line 1] compile error: expected '(' after 'for'");
    CHECK(error_of("for (;i) {}") == "[line 1] compile error: expected ';' after loop condition");
    CHECK(error_of("for (;;i {}") == "[line 1] compile error: expected ')' after for clauses");
    CHECK(error_of("for (let i = 0 i < 3;;) {}") ==
          "[line 1] compile error: expected ';' after variable declaration");
    // Declarations are not statements: they cannot be the body of an if.
    CHECK(error_of("if (x) let y = 1;") == "[line 1] compile error: expected expression");
    CHECK(error_of("if (x) fn f() {}") == "[line 1] compile error: expected expression");
}

TEST_CASE("invalid assignment targets") {
    CHECK(error_of("1 = 2;") == "[line 1] compile error: invalid assignment target");
    CHECK(error_of("a + b = c;") == "[line 1] compile error: invalid assignment target");
    CHECK(error_of("-a = 1;") == "[line 1] compile error: invalid assignment target");
    CHECK(error_of("f() = 1;") == "[line 1] compile error: invalid assignment target");
    CHECK(error_of("a and b = 1;") == "[line 1] compile error: invalid assignment target");
    CHECK(error_of("[1] = 2;") == "[line 1] compile error: invalid assignment target");
    // Reported on the line of the '='.
    CHECK(error_of("a\n=\n1 = 2;") == "[line 3] compile error: invalid assignment target");
}

TEST_CASE("lexer errors surface through parse") {
    CHECK(error_of("print @;") == "[line 1] compile error: unexpected character '@'");
}

TEST_CASE("nesting limit") {
    // 200 levels of parentheses are fine; 201 are too deep.
    CHECK(error_of("print " + repeat("(", 200) + "1" + repeat(")", 200) + ";") == "no error");
    CHECK(error_of("print " + repeat("(", 201) + "1" + repeat(")", 201) + ";") ==
          "[line 1] compile error: nesting too deep");
    // Blocks, unary operators, arrays, and if bodies count too.
    CHECK(error_of(repeat("{", 200) + repeat("}", 200)) == "no error");
    CHECK(error_of(repeat("{", 201) + repeat("}", 201)) ==
          "[line 1] compile error: nesting too deep");
    CHECK(error_of("print " + repeat("-", 200) + "x;") == "no error");
    CHECK(error_of("print " + repeat("-", 201) + "x;") ==
          "[line 1] compile error: nesting too deep");
    CHECK(error_of("print " + repeat("[", 201) + repeat("]", 201) + ";") ==
          "[line 1] compile error: nesting too deep");
    CHECK(error_of(repeat("if (x) ", 201) + "print 1;") ==
          "[line 1] compile error: nesting too deep");
    // A very deep input must be an error, not a stack overflow.
    CHECK(error_of("print " + repeat("(", 100000) + "1;") ==
          "[line 1] compile error: nesting too deep");
    // Long flat chains are not nesting.
    CHECK(error_of("print 1" + repeat(" + 1", 5000) + ";") == "no error");
    CHECK(error_of("f" + repeat("(1)", 5000) + ";") == "no error");
}

// A chain is not syntactic nesting, so the nesting limit lets it grow without bound (notes §2.6).
// Building, dumping, and destroying it must not use native stack per link. The length is far past
// what an ordinary or ThreadSanitizer stack survives with one recursive frame set per link.
TEST_CASE("very long flat chains") {
    const int n = 100000;
    struct Case {
        std::string source;
        std::string head;  // dump prefix that the chain's outermost nodes produce
    };
    const Case cases[] = {
        {"print 1" + repeat(" + 1", n) + ";", "(print " + repeat("(+ ", n) + "1"},
        {"print 1" + repeat(" and 1", n) + ";", "(print " + repeat("(and ", n) + "1"},
        {"f" + repeat("(1)", n) + ";", "(expr " + repeat("(call ", n) + "f"},
        {"a" + repeat("[0]", n) + ";", "(expr " + repeat("(index ", n) + "a"},
        {"a" + repeat("[0]", n) + " = 1;", "(expr (set-index " + repeat("(index ", n - 1) + "a"},
    };
    for (const Case& c : cases) {
        rung::ParseResult result = parse(c.source);
        REQUIRE(result.ok());
        std::string out = rung::dump_ast(*result.program);
        CHECK(out.compare(0, c.head.size(), c.head) == 0);
        // Leaving this scope destroys the whole tree.
    }
}

TEST_CASE("chain dump keeps the shape of small chains") {
    CHECK(dump("print 1 + 2 - 3;") == "(print (- (+ 1 2) 3))");
    CHECK(dump("a[0](1, 2)[3] = 4;") == "(expr (set-index (call (index a 0) 1 2) 3 4))");
    CHECK(dump("f(1)(2)(3);") == "(expr (call (call (call f 1) 2) 3))");
    CHECK(dump("a or b and c or d;") == "(expr (or (or a (and b c)) d))");
}
