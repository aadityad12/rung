#include "token.h"

namespace rung {

std::string_view token_kind_name(TokenKind kind) {
    switch (kind) {
        case TokenKind::LeftParen: return "LeftParen";
        case TokenKind::RightParen: return "RightParen";
        case TokenKind::LeftBrace: return "LeftBrace";
        case TokenKind::RightBrace: return "RightBrace";
        case TokenKind::LeftBracket: return "LeftBracket";
        case TokenKind::RightBracket: return "RightBracket";
        case TokenKind::Comma: return "Comma";
        case TokenKind::Semicolon: return "Semicolon";
        case TokenKind::Plus: return "Plus";
        case TokenKind::Minus: return "Minus";
        case TokenKind::Star: return "Star";
        case TokenKind::Slash: return "Slash";
        case TokenKind::Percent: return "Percent";
        case TokenKind::Bang: return "Bang";
        case TokenKind::BangEqual: return "BangEqual";
        case TokenKind::Equal: return "Equal";
        case TokenKind::EqualEqual: return "EqualEqual";
        case TokenKind::Less: return "Less";
        case TokenKind::LessEqual: return "LessEqual";
        case TokenKind::Greater: return "Greater";
        case TokenKind::GreaterEqual: return "GreaterEqual";
        case TokenKind::Identifier: return "Identifier";
        case TokenKind::Int: return "Int";
        case TokenKind::Float: return "Float";
        case TokenKind::String: return "String";
        case TokenKind::And: return "And";
        case TokenKind::Else: return "Else";
        case TokenKind::False: return "False";
        case TokenKind::Fn: return "Fn";
        case TokenKind::For: return "For";
        case TokenKind::If: return "If";
        case TokenKind::Let: return "Let";
        case TokenKind::Nil: return "Nil";
        case TokenKind::Or: return "Or";
        case TokenKind::Print: return "Print";
        case TokenKind::Return: return "Return";
        case TokenKind::True: return "True";
        case TokenKind::While: return "While";
        case TokenKind::Eof: return "Eof";
    }
    return "Unknown";
}

}  // namespace rung
