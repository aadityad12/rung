#include "parser.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "lexer.h"

namespace rung {
namespace {

// Thrown to unwind out of the recursive descent at the first error. Never escapes parse().
struct ParseAbort {
    CompileError error;
};

// The one Int token value that is only legal after a unary minus (notes D9).
constexpr std::int64_t kIntMinMagnitude = 2147483648;

struct BinaryInfo {
    int precedence;  // higher binds tighter
    bool logical;
    BinaryOp binary;
    LogicalOp logic;
};

// Precedence-climbing table (lowest to highest): or, and, equality, comparison, term, factor.
// All of them are left-associative. Returns false for tokens that are not binary operators.
bool binary_info(TokenKind kind, BinaryInfo& info) {
    switch (kind) {
        case TokenKind::Or: info = {1, true, BinaryOp::Add, LogicalOp::Or}; return true;
        case TokenKind::And: info = {2, true, BinaryOp::Add, LogicalOp::And}; return true;
        case TokenKind::EqualEqual: info = {3, false, BinaryOp::Eq, LogicalOp::And}; return true;
        case TokenKind::BangEqual: info = {3, false, BinaryOp::Ne, LogicalOp::And}; return true;
        case TokenKind::Less: info = {4, false, BinaryOp::Lt, LogicalOp::And}; return true;
        case TokenKind::LessEqual: info = {4, false, BinaryOp::Le, LogicalOp::And}; return true;
        case TokenKind::Greater: info = {4, false, BinaryOp::Gt, LogicalOp::And}; return true;
        case TokenKind::GreaterEqual:
            info = {4, false, BinaryOp::Ge, LogicalOp::And};
            return true;
        case TokenKind::Plus: info = {5, false, BinaryOp::Add, LogicalOp::And}; return true;
        case TokenKind::Minus: info = {5, false, BinaryOp::Sub, LogicalOp::And}; return true;
        case TokenKind::Star: info = {6, false, BinaryOp::Mul, LogicalOp::And}; return true;
        case TokenKind::Slash: info = {6, false, BinaryOp::Div, LogicalOp::And}; return true;
        case TokenKind::Percent: info = {6, false, BinaryOp::Mod, LogicalOp::And}; return true;
        default: return false;
    }
}

class Parser {
public:
    Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

    std::vector<StmtPtr> parse_program() {
        std::vector<StmtPtr> statements;
        while (!check(TokenKind::Eof)) statements.push_back(declaration());
        return statements;
    }

private:
    // Counts one level of nesting for as long as it lives (notes §2.6). Only constructs that
    // actually nest count, so a plain top-level statement or expression is level zero.
    class Nest {
    public:
        explicit Nest(Parser& parser) : parser_(parser) {
            if (++parser_.depth_ > kMaxNesting) parser_.fail("nesting too deep");
        }
        ~Nest() { --parser_.depth_; }
        Nest(const Nest&) = delete;
        Nest& operator=(const Nest&) = delete;

    private:
        Parser& parser_;
    };

    // ---- token helpers ----

    const Token& peek() const { return tokens_[pos_]; }
    const Token& previous() const { return tokens_[pos_ - 1]; }
    bool check(TokenKind kind) const { return peek().kind == kind; }

    const Token& advance() {
        if (!check(TokenKind::Eof)) ++pos_;  // never step past Eof
        return previous();
    }

    bool match(TokenKind kind) {
        if (!check(kind)) return false;
        advance();
        return true;
    }

    [[noreturn]] void fail(std::string message) const {
        throw ParseAbort{CompileError{peek().line, std::move(message)}};
    }

    const Token& consume(TokenKind kind, const char* message) {
        if (!check(kind)) fail(message);
        return advance();
    }

    // ---- statements ----

    StmtPtr declaration() {
        if (match(TokenKind::Fn)) return function_declaration();
        if (match(TokenKind::Let)) return let_declaration();
        return statement();
    }

    StmtPtr function_declaration() {
        int line = previous().line;
        std::string_view name =
            consume(TokenKind::Identifier, "expected function name after 'fn'").lexeme;
        consume(TokenKind::LeftParen, "expected '(' after function name");
        std::vector<std::string_view> params;
        if (!check(TokenKind::RightParen)) {
            do {
                params.push_back(consume(TokenKind::Identifier, "expected parameter name").lexeme);
            } while (match(TokenKind::Comma));
        }
        consume(TokenKind::RightParen, "expected ')' after parameters");
        consume_block_open("expected '{' before function body");
        Block body = block_rest(previous().line);
        return std::make_unique<Stmt>(Function{name, std::move(params), std::move(body), line});
    }

    StmtPtr let_declaration() {
        int line = previous().line;
        std::string_view name =
            consume(TokenKind::Identifier, "expected variable name after 'let'").lexeme;
        ExprPtr init;
        if (match(TokenKind::Equal)) init = expression();
        consume(TokenKind::Semicolon, "expected ';' after variable declaration");
        return std::make_unique<Stmt>(Let{name, std::move(init), line});
    }

    void consume_block_open(const char* message) { consume(TokenKind::LeftBrace, message); }

    // After the '{' has been consumed: declarations up to the matching '}'.
    Block block_rest(int line) {
        Nest nest(*this);
        Block block{{}, line};
        while (!check(TokenKind::RightBrace) && !check(TokenKind::Eof)) {
            block.statements.push_back(declaration());
        }
        consume(TokenKind::RightBrace, "expected '}' after block");
        return block;
    }

    // The body of an if/while/for. Counted as a level, so `if (a) if (b) if (c) ...` is bounded.
    StmtPtr nested_statement() {
        Nest nest(*this);
        return statement();
    }

    StmtPtr statement() {
        if (match(TokenKind::Print)) return print_statement();
        if (match(TokenKind::If)) return if_statement();
        if (match(TokenKind::While)) return while_statement();
        if (match(TokenKind::For)) return for_statement();
        if (match(TokenKind::Return)) return return_statement();
        if (match(TokenKind::LeftBrace)) {
            int line = previous().line;
            return std::make_unique<Stmt>(block_rest(line));
        }
        return expression_statement();
    }

    StmtPtr print_statement() {
        int line = previous().line;
        ExprPtr value = expression();
        consume(TokenKind::Semicolon, "expected ';' after value");
        return std::make_unique<Stmt>(Print{std::move(value), line});
    }

    StmtPtr expression_statement() {
        int line = peek().line;
        ExprPtr expr = expression();
        consume(TokenKind::Semicolon, "expected ';' after expression");
        return std::make_unique<Stmt>(ExprStmt{std::move(expr), line});
    }

    StmtPtr if_statement() {
        int line = previous().line;
        consume(TokenKind::LeftParen, "expected '(' after 'if'");
        ExprPtr condition = expression();
        consume(TokenKind::RightParen, "expected ')' after condition");
        StmtPtr then_branch = nested_statement();
        StmtPtr else_branch;
        // Taking `else` here, greedily, is what binds it to the nearest `if`.
        if (match(TokenKind::Else)) else_branch = nested_statement();
        return std::make_unique<Stmt>(
            If{std::move(condition), std::move(then_branch), std::move(else_branch), line});
    }

    StmtPtr while_statement() {
        int line = previous().line;
        consume(TokenKind::LeftParen, "expected '(' after 'while'");
        ExprPtr condition = expression();
        consume(TokenKind::RightParen, "expected ')' after condition");
        StmtPtr body = nested_statement();
        return std::make_unique<Stmt>(While{std::move(condition), std::move(body), line});
    }

    // `for (init; cond; inc) body` becomes
    //   Block{ init; While(cond or true, Block{ body; inc }) }
    // so the loop variable is one variable shared by every iteration (notes §2.6). The outer
    // Block is always emitted (even with no init) so the shape never depends on the clauses.
    StmtPtr for_statement() {
        int line = previous().line;
        consume(TokenKind::LeftParen, "expected '(' after 'for'");

        StmtPtr init;
        if (match(TokenKind::Semicolon)) {
            // no initializer
        } else if (match(TokenKind::Let)) {
            init = let_declaration();
        } else {
            init = expression_statement();
        }

        ExprPtr condition;
        if (!check(TokenKind::Semicolon)) condition = expression();
        consume(TokenKind::Semicolon, "expected ';' after loop condition");

        ExprPtr increment;
        int increment_line = peek().line;
        if (!check(TokenKind::RightParen)) increment = expression();
        consume(TokenKind::RightParen, "expected ')' after for clauses");

        StmtPtr body = nested_statement();

        Block inner{{}, line};
        inner.statements.push_back(std::move(body));
        if (increment) {
            inner.statements.push_back(
                std::make_unique<Stmt>(ExprStmt{std::move(increment), increment_line}));
        }
        if (!condition) {
            condition = std::make_unique<Expr>(Literal{true, line});
        }
        Block outer{{}, line};
        if (init) outer.statements.push_back(std::move(init));
        outer.statements.push_back(std::make_unique<Stmt>(
            While{std::move(condition), std::make_unique<Stmt>(std::move(inner)), line}));
        return std::make_unique<Stmt>(std::move(outer));
    }

    StmtPtr return_statement() {
        int line = previous().line;
        ExprPtr value;
        if (!check(TokenKind::Semicolon)) value = expression();
        consume(TokenKind::Semicolon, "expected ';' after return value");
        return std::make_unique<Stmt>(Return{std::move(value), line});
    }

    // ---- expressions ----

    ExprPtr expression() { return assignment(); }

    // Parse the left side as an ordinary expression first; only when an '=' follows do we check
    // that what we parsed is a legal target. That keeps the grammar LL(1) with no backtracking.
    ExprPtr assignment() {
        ExprPtr left = binary(1);
        if (!check(TokenKind::Equal)) return left;

        int equals_line = peek().line;
        advance();
        if (auto* var = std::get_if<Variable>(&left->node)) {
            ExprPtr value = assignment_rhs();
            return std::make_unique<Expr>(
                Assign{var->name, std::move(value), Binding{}, var->line});
        }
        if (auto* index = std::get_if<Index>(&left->node)) {
            ExprPtr value = assignment_rhs();
            return std::make_unique<Expr>(IndexAssign{
                std::move(index->object), std::move(index->index), std::move(value), index->line});
        }
        throw ParseAbort{CompileError{equals_line, "invalid assignment target"}};
    }

    // Right-associative: `a = b = c` recurses here, and each step is a nesting level.
    ExprPtr assignment_rhs() {
        Nest nest(*this);
        return assignment();
    }

    // Precedence climbing (Pratt style): parse an operand, then keep folding in operators whose
    // precedence is at least `min_precedence`. The right operand is parsed one level tighter,
    // which makes every operator left-associative. The same idea as the Pratt parser in
    // Crafting Interpreters' clox, but driven by a small table and a loop instead of one
    // function per precedence level.
    ExprPtr binary(int min_precedence) {
        ExprPtr left = unary();
        BinaryInfo info{};
        while (binary_info(peek().kind, info) && info.precedence >= min_precedence) {
            int line = advance().line;
            ExprPtr right = binary(info.precedence + 1);
            if (info.logical) {
                left = std::make_unique<Expr>(
                    Logical{info.logic, std::move(left), std::move(right), line});
            } else {
                left = std::make_unique<Expr>(
                    Binary{info.binary, std::move(left), std::move(right), line});
            }
        }
        return left;
    }

    ExprPtr unary() {
        if (check(TokenKind::Bang) || check(TokenKind::Minus)) {
            const Token& op = advance();
            int line = op.line;
            UnaryOp kind = op.kind == TokenKind::Bang ? UnaryOp::Not : UnaryOp::Negate;

            // `-2147483648`: the lexer allows that magnitude, and only here does it become
            // INT32_MIN (notes D9). If a call or index follows, the literal is really the
            // operand of that postfix, so we leave it alone and let primary() reject it.
            if (kind == UnaryOp::Negate && check(TokenKind::Int) &&
                peek().int_value == kIntMinMagnitude) {
                TokenKind after = tokens_[pos_ + 1].kind;  // Eof always follows, so in range
                if (after != TokenKind::LeftParen && after != TokenKind::LeftBracket) {
                    advance();
                    return std::make_unique<Expr>(
                        Literal{std::numeric_limits<std::int32_t>::min(), line});
                }
            }

            Nest nest(*this);
            ExprPtr operand = unary();
            return std::make_unique<Expr>(Unary{kind, std::move(operand), line});
        }
        return postfix();
    }

    ExprPtr postfix() {
        ExprPtr expr = primary();
        for (;;) {
            if (match(TokenKind::LeftParen)) {
                int line = previous().line;
                std::vector<ExprPtr> args;
                {
                    Nest nest(*this);
                    if (!check(TokenKind::RightParen)) {
                        do {
                            args.push_back(expression());
                        } while (match(TokenKind::Comma));
                    }
                }
                consume(TokenKind::RightParen, "expected ')' after arguments");
                expr = std::make_unique<Expr>(Call{std::move(expr), std::move(args), line});
            } else if (match(TokenKind::LeftBracket)) {
                int line = previous().line;
                ExprPtr index;
                {
                    Nest nest(*this);
                    index = expression();
                }
                consume(TokenKind::RightBracket, "expected ']' after index");
                expr = std::make_unique<Expr>(Index{std::move(expr), std::move(index), line});
            } else {
                return expr;
            }
        }
    }

    ExprPtr primary() {
        const Token& token = peek();
        switch (token.kind) {
            case TokenKind::Int: {
                if (token.int_value >= kIntMinMagnitude) {
                    fail("integer literal '" + std::to_string(token.int_value) +
                         "' is too large for a 32-bit int");
                }
                advance();
                return std::make_unique<Expr>(
                    Literal{static_cast<std::int32_t>(token.int_value), token.line});
            }
            case TokenKind::Float:
                advance();
                return std::make_unique<Expr>(Literal{token.float_value, token.line});
            case TokenKind::String:
                advance();
                return std::make_unique<Expr>(Literal{token.string_value, token.line});
            case TokenKind::True:
                advance();
                return std::make_unique<Expr>(Literal{true, token.line});
            case TokenKind::False:
                advance();
                return std::make_unique<Expr>(Literal{false, token.line});
            case TokenKind::Nil:
                advance();
                return std::make_unique<Expr>(Literal{std::monostate{}, token.line});
            case TokenKind::Identifier:
                advance();
                return std::make_unique<Expr>(Variable{token.lexeme, Binding{}, token.line});
            case TokenKind::LeftParen: {
                advance();
                Nest nest(*this);
                ExprPtr inner = expression();
                consume(TokenKind::RightParen, "expected ')' after expression");
                return inner;  // no grouping node: parentheses only affect the shape
            }
            case TokenKind::LeftBracket: {
                advance();
                int line = token.line;
                Nest nest(*this);
                std::vector<ExprPtr> elements;
                if (!check(TokenKind::RightBracket)) {
                    do {
                        elements.push_back(expression());
                    } while (match(TokenKind::Comma));
                }
                consume(TokenKind::RightBracket, "expected ']' after array elements");
                return std::make_unique<Expr>(ArrayLiteral{std::move(elements), line});
            }
            default:
                fail("expected expression");
        }
    }

    const std::vector<Token>& tokens_;
    std::size_t pos_ = 0;
    int depth_ = 0;
};

}  // namespace

ParseResult parse(std::string source) {
    ParseResult result;
    auto program = std::make_unique<Program>();
    program->source = std::move(source);

    // The tokens point into program->source, which now has its final address.
    LexResult lexed = lex(program->source);
    if (!lexed.ok()) {
        result.error = std::move(lexed.error);
        return result;
    }
    program->tokens = std::move(lexed.tokens);

    try {
        Parser parser(program->tokens);
        program->statements = parser.parse_program();
    } catch (ParseAbort& abort) {
        result.error = std::move(abort.error);
        return result;
    }
    result.program = std::move(program);
    return result;
}

}  // namespace rung
