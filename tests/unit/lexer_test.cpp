#include <doctest.h>

#include <string>
#include <string_view>

#include "lexer.h"

using rung::lex;
using rung::LexResult;
using rung::TokenKind;

namespace {

// Token kinds as one space-separated string, so a failing CHECK prints something readable.
std::string kinds(std::string_view source) {
    LexResult result = lex(source);
    if (!result.ok()) return "ERROR: " + rung::format_error(*result.error);
    std::string out;
    for (const rung::Token& token : result.tokens) {
        if (!out.empty()) out += ' ';
        out += rung::token_kind_name(token.kind);
    }
    return out;
}

std::string error_of(std::string_view source) {
    LexResult result = lex(source);
    return result.ok() ? "no error" : rung::format_error(*result.error);
}

}  // namespace

TEST_CASE("empty input is just Eof") {
    CHECK(kinds("") == "Eof");
    CHECK(kinds("   \n\t  // only a comment") == "Eof");
}

TEST_CASE("punctuation and operators") {
    CHECK(kinds("( ) { } [ ] , ;") ==
          "LeftParen RightParen LeftBrace RightBrace LeftBracket RightBracket Comma Semicolon Eof");
    CHECK(kinds("+ - * / %") == "Plus Minus Star Slash Percent Eof");
    CHECK(kinds("! != = == < <= > >=") ==
          "Bang BangEqual Equal EqualEqual Less LessEqual Greater GreaterEqual Eof");
}

TEST_CASE("two-character operators are matched greedily") {
    CHECK(kinds("a==b") == "Identifier EqualEqual Identifier Eof");
    CHECK(kinds("a===b") == "Identifier EqualEqual Equal Identifier Eof");
    CHECK(kinds("!!=") == "Bang BangEqual Eof");
}

TEST_CASE("keywords versus identifiers") {
    CHECK(kinds("and else false fn for if let nil or print return true while") ==
          "And Else False Fn For If Let Nil Or Print Return True While Eof");
    // A keyword prefix or suffix inside a longer name is still an identifier.
    CHECK(kinds("fnord letter iffy printer nil_ _while while2") ==
          "Identifier Identifier Identifier Identifier Identifier Identifier Identifier Eof");
    // Native functions are not keywords.
    CHECK(kinds("array len clock") == "Identifier Identifier Identifier Eof");
    // Case matters.
    CHECK(kinds("Let TRUE") == "Identifier Identifier Eof");
}

TEST_CASE("integer literals") {
    LexResult result = lex("0 42 007 2147483647 2147483648");
    REQUIRE(result.ok());
    REQUIRE(result.tokens.size() == 6);
    CHECK(result.tokens[0].int_value == 0);
    CHECK(result.tokens[1].int_value == 42);
    CHECK(result.tokens[2].int_value == 7);
    CHECK(result.tokens[3].int_value == 2147483647);
    // Accepted by the lexer so that -2147483648 can be written; the parser restricts it.
    CHECK(result.tokens[4].int_value == 2147483648);
}

TEST_CASE("integer literals too large for 32 bits are rejected") {
    CHECK(error_of("2147483649") ==
          "[line 1] compile error: integer literal '2147483649' is too large for a 32-bit int");
    CHECK(error_of("\n\n99999999999999999999999") ==
          "[line 3] compile error: integer literal '99999999999999999999999' is too large for a "
          "32-bit int");
}

TEST_CASE("float literals") {
    LexResult result = lex("1.5 0.25 3.0");
    REQUIRE(result.ok());
    CHECK(kinds("1.5 0.25 3.0") == "Float Float Float Eof");
    CHECK(result.tokens[0].float_value == 1.5);
    CHECK(result.tokens[1].float_value == 0.25);
    CHECK(result.tokens[2].float_value == 3.0);
}

TEST_CASE("unary minus is a separate token, not part of the literal") {
    CHECK(kinds("-1") == "Minus Int Eof");
    CHECK(kinds("-2147483648") == "Minus Int Eof");
    CHECK(kinds("a-1") == "Identifier Minus Int Eof");
}

TEST_CASE("malformed floats") {
    // No digits after the dot: the int lexes, then the dot is an unexpected character.
    CHECK(error_of("1.") == "[line 1] compile error: unexpected character '.'");
    CHECK(error_of(".5") == "[line 1] compile error: unexpected character '.'");
    // No exponents.
    CHECK(kinds("1e5") == "Int Identifier Eof");
}

TEST_CASE("strings and escapes") {
    LexResult result = lex(R"("hello" "" "a\nb\t\"q\"\\")");
    REQUIRE(result.ok());
    REQUIRE(result.tokens.size() == 4);
    CHECK(result.tokens[0].string_value == "hello");
    CHECK(result.tokens[0].lexeme == "\"hello\"");
    CHECK(result.tokens[1].string_value.empty());
    CHECK(result.tokens[2].string_value == "a\nb\t\"q\"\\");
}

TEST_CASE("a comment marker inside a string is just text") {
    LexResult result = lex(R"("not // a comment")");
    REQUIRE(result.ok());
    CHECK(result.tokens[0].string_value == "not // a comment");
}

TEST_CASE("strings may span lines; the token keeps its starting line") {
    LexResult result = lex("\"one\ntwo\" x");
    REQUIRE(result.ok());
    CHECK(result.tokens[0].string_value == "one\ntwo");
    CHECK(result.tokens[0].line == 1);
    CHECK(result.tokens[1].line == 2);  // x comes after the newline inside the string
}

TEST_CASE("string errors") {
    CHECK(error_of("\"abc") == "[line 1] compile error: unterminated string");
    CHECK(error_of("x\n\"abc\ndef") == "[line 2] compile error: unterminated string");
    CHECK(error_of(R"("bad \q escape")") ==
          "[line 1] compile error: invalid escape sequence '\\q'");
    CHECK(error_of("\"ends in backslash\\") == "[line 1] compile error: unterminated string");
}

TEST_CASE("line numbers") {
    LexResult result = lex("let a = 1;\n\n// comment\nprint a;");
    REQUIRE(result.ok());
    CHECK(result.tokens[0].line == 1);  // let
    CHECK(result.tokens[5].line == 4);  // print
    CHECK(result.tokens.back().kind == TokenKind::Eof);
    CHECK(result.tokens.back().line == 4);
}

TEST_CASE("unexpected characters stop lexing at the first error") {
    CHECK(error_of("let x = 1 @ 2;") == "[line 1] compile error: unexpected character '@'");
    CHECK(error_of("ok\n#") == "[line 2] compile error: unexpected character '#'");
    CHECK(error_of("caf\xc3\xa9") == "[line 1] compile error: unexpected byte 195");
    CHECK(lex("@").tokens.empty());
}

TEST_CASE("a small program") {
    CHECK(kinds("fn add(a, b) { return a + b; }\nprint add(1, 2.5);") ==
          "Fn Identifier LeftParen Identifier Comma Identifier RightParen LeftBrace Return "
          "Identifier Plus Identifier Semicolon RightBrace Print Identifier LeftParen Int Comma "
          "Float RightParen Semicolon Eof");
    CHECK(kinds("let a = [1, 2]; a[0] = len(a);") ==
          "Let Identifier Equal LeftBracket Int Comma Int RightBracket Semicolon Identifier "
          "LeftBracket Int RightBracket Equal Identifier LeftParen Identifier RightParen "
          "Semicolon Eof");
}
