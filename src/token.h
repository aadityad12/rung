#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rung {

enum class TokenKind : std::uint8_t {
    // Single-character punctuation.
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Comma,
    Semicolon,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,

    // Operators that may be one or two characters.
    Bang,
    BangEqual,
    Equal,
    EqualEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,

    // Literals.
    Identifier,
    Int,
    Float,
    String,

    // Keywords.
    And,
    Else,
    False,
    Fn,
    For,
    If,
    Let,
    Nil,
    Or,
    Print,
    Return,
    True,
    While,

    Eof,
};

std::string_view token_kind_name(TokenKind kind);

struct Token {
    TokenKind kind;
    std::string_view lexeme;  // exact source text; points into the caller's source buffer
    int line;                 // line the token starts on (1-based)

    // Decoded literal payloads. Only the one matching `kind` is meaningful.
    std::int64_t int_value = 0;  // Int: 0 .. 2147483648 (see docs/notes.md D9)
    double float_value = 0.0;    // Float
    std::string string_value{};  // String, with escapes already decoded
};

}  // namespace rung
