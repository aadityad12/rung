#include "lexer.h"

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <system_error>

namespace rung {
namespace {

// Largest literal the lexer accepts. It is one past INT32_MAX so that `-2147483648` can be
// written; the parser rejects this value anywhere except directly after unary minus.
constexpr std::int64_t kMaxIntLiteral = 2147483648;

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

struct Keyword {
    std::string_view text;
    TokenKind kind;
};

constexpr Keyword kKeywords[] = {
    {"and", TokenKind::And},     {"else", TokenKind::Else},     {"false", TokenKind::False},
    {"fn", TokenKind::Fn},       {"for", TokenKind::For},       {"if", TokenKind::If},
    {"let", TokenKind::Let},     {"nil", TokenKind::Nil},       {"or", TokenKind::Or},
    {"print", TokenKind::Print}, {"return", TokenKind::Return}, {"true", TokenKind::True},
    {"while", TokenKind::While},
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    LexResult run() {
        while (!result_.error) {
            skip_whitespace_and_comments();
            if (result_.error) break;
            start_ = current_;
            start_line_ = line_;
            if (at_end()) {
                add(TokenKind::Eof);
                break;
            }
            scan_token();
        }
        if (result_.error) result_.tokens.clear();
        return std::move(result_);
    }

private:
    std::string_view source_;
    std::size_t start_ = 0;    // first character of the token being scanned
    std::size_t current_ = 0;  // next character to read
    int line_ = 1;
    int start_line_ = 1;
    LexResult result_;

    bool at_end() const { return current_ >= source_.size(); }
    char peek() const { return at_end() ? '\0' : source_[current_]; }
    char peek_next() const {
        return current_ + 1 >= source_.size() ? '\0' : source_[current_ + 1];
    }
    char advance() { return source_[current_++]; }

    bool match(char expected) {
        if (peek() != expected) return false;
        ++current_;
        return true;
    }

    Token& add(TokenKind kind) {
        result_.tokens.push_back(
            Token{kind, source_.substr(start_, current_ - start_), start_line_});
        return result_.tokens.back();
    }

    void fail(int line, std::string message) {
        result_.error = CompileError{line, std::move(message)};
    }

    void skip_whitespace_and_comments() {
        while (!at_end()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r') {
                ++current_;
            } else if (c == '\n') {
                ++line_;
                ++current_;
            } else if (c == '/' && peek_next() == '/') {
                while (!at_end() && peek() != '\n') ++current_;
            } else {
                return;
            }
        }
    }

    void scan_token() {
        char c = advance();
        if (is_digit(c)) return number();
        if (is_alpha(c)) return identifier();

        switch (c) {
            case '(': add(TokenKind::LeftParen); return;
            case ')': add(TokenKind::RightParen); return;
            case '{': add(TokenKind::LeftBrace); return;
            case '}': add(TokenKind::RightBrace); return;
            case '[': add(TokenKind::LeftBracket); return;
            case ']': add(TokenKind::RightBracket); return;
            case ',': add(TokenKind::Comma); return;
            case ';': add(TokenKind::Semicolon); return;
            case '+': add(TokenKind::Plus); return;
            case '-': add(TokenKind::Minus); return;
            case '*': add(TokenKind::Star); return;
            case '/': add(TokenKind::Slash); return;
            case '%': add(TokenKind::Percent); return;
            case '!': add(match('=') ? TokenKind::BangEqual : TokenKind::Bang); return;
            case '=': add(match('=') ? TokenKind::EqualEqual : TokenKind::Equal); return;
            case '<': add(match('=') ? TokenKind::LessEqual : TokenKind::Less); return;
            case '>': add(match('=') ? TokenKind::GreaterEqual : TokenKind::Greater); return;
            case '"': string(); return;
            default: break;
        }

        // Printable ASCII is shown as itself; anything else (control bytes, UTF-8) as a code.
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7f) {
            fail(line_, std::string("unexpected character '") + c + "'");
        } else {
            fail(line_, "unexpected byte " + std::to_string(u));
        }
    }

    void identifier() {
        while (is_alnum(peek())) ++current_;
        std::string_view text = source_.substr(start_, current_ - start_);
        for (const Keyword& keyword : kKeywords) {
            if (keyword.text == text) {
                add(keyword.kind);
                return;
            }
        }
        add(TokenKind::Identifier);
    }

    void number() {
        while (is_digit(peek())) ++current_;

        // A float needs digits on both sides of the dot. `1.` is an int followed by a stray
        // '.', which the next scan reports as an unexpected character.
        if (peek() == '.' && is_digit(peek_next())) {
            ++current_;  // the '.'
            while (is_digit(peek())) ++current_;
            // strtod, not std::from_chars: older Apple libc++ (Xcode 16) lacks the
            // floating-point overload. The text is plain digits and one '.', and Rung never
            // calls setlocale, so strtod's locale sensitivity can't bite here.
            std::string text(source_.substr(start_, current_ - start_));
            char* end = nullptr;
            errno = 0;
            double value = std::strtod(text.c_str(), &end);
            if (errno == ERANGE || end != text.c_str() + text.size()) {
                fail(start_line_, "invalid float literal '" + text + "'");
                return;
            }
            add(TokenKind::Float).float_value = value;
            return;
        }

        std::string_view text = source_.substr(start_, current_ - start_);
        std::int64_t value = 0;
        auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec == std::errc::result_out_of_range || value > kMaxIntLiteral) {
            fail(start_line_, "integer literal '" + std::string(text) +
                                  "' is too large for a 32-bit int");
            return;
        }
        add(TokenKind::Int).int_value = value;
    }

    void string() {
        std::string decoded;
        while (!at_end() && peek() != '"') {
            char c = advance();
            if (c == '\n') {
                ++line_;
                decoded += c;
                continue;
            }
            if (c != '\\') {
                decoded += c;
                continue;
            }
            if (at_end()) break;  // reported below as unterminated
            char escape = advance();
            switch (escape) {
                case 'n': decoded += '\n'; break;
                case 't': decoded += '\t'; break;
                case '"': decoded += '"'; break;
                case '\\': decoded += '\\'; break;
                default:
                    if (escape == '\n') ++line_;
                    fail(line_, std::string("invalid escape sequence '\\") + escape + "'");
                    return;
            }
        }
        if (at_end()) {
            fail(start_line_, "unterminated string");
            return;
        }
        ++current_;  // closing quote
        add(TokenKind::String).string_value = std::move(decoded);
    }
};

}  // namespace

LexResult lex(std::string_view source) { return Lexer(source).run(); }

}  // namespace rung
