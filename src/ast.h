#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "token.h"

namespace rung {

// The syntax tree. Nodes are plain structs collected in a std::variant (no virtual classes), so
// an engine walks them with std::visit and the compiler checks that every node kind is handled.
// Children are owned through unique_ptr. Every node carries `line`: the line of its defining
// token (the operator, a call's `(`, an index's `[`, a statement's keyword), which is the line
// runtime errors report (docs/notes.md §2.5).
//
// Names are string_views into the source text owned by Program, so a Program must outlive
// every engine that runs it.

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// Filled in by the resolver (notes D11); the parser leaves it Unresolved.
enum class BindingKind : std::uint8_t { Unresolved, Global, Local };
struct Binding {
    BindingKind kind = BindingKind::Unresolved;
    int hops = 0;  // scopes to walk up, for Local
};

// ---- Expressions ----

struct Literal {
    // monostate is `nil`. Ints are already 32-bit: the parser range-checked them (notes D9).
    std::variant<std::monostate, bool, std::int32_t, double, std::string> value;
    int line;
};

struct Variable {
    std::string_view name;
    Binding binding;
    int line;  // the identifier
};

struct Assign {
    std::string_view name;
    ExprPtr value;
    Binding binding;
    int line;  // the identifier
};

enum class UnaryOp : std::uint8_t { Negate, Not };
struct Unary {
    UnaryOp op;
    ExprPtr operand;
    int line;
};

enum class BinaryOp : std::uint8_t { Add, Sub, Mul, Div, Mod, Eq, Ne, Lt, Le, Gt, Ge };
struct Binary {
    BinaryOp op;
    ExprPtr left;
    ExprPtr right;
    int line;
};

// Separate from Binary because `and` / `or` short-circuit (notes §2.2).
enum class LogicalOp : std::uint8_t { And, Or };
struct Logical {
    LogicalOp op;
    ExprPtr left;
    ExprPtr right;
    int line;
};

struct Call {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    int line;  // the '('
};

struct ArrayLiteral {
    std::vector<ExprPtr> elements;
    int line;  // the '['
};

struct Index {
    ExprPtr object;
    ExprPtr index;
    int line;  // the '['
};

struct IndexAssign {
    ExprPtr object;
    ExprPtr index;
    ExprPtr value;
    int line;  // the '['
};

struct Expr {
    using Node = std::variant<Literal, Variable, Assign, Unary, Binary, Logical, Call,
                              ArrayLiteral, Index, IndexAssign>;
    Node node;
    Expr(Node n) : node(std::move(n)) {}  // implicit on purpose: make_unique<Expr>(Literal{...})
};

// ---- Statements ----

struct Print {
    ExprPtr value;
    int line;
};

struct ExprStmt {
    ExprPtr expr;
    int line;
};

struct Let {
    std::string_view name;
    ExprPtr init;  // null for `let x;`
    int line;
};

struct Block {
    std::vector<StmtPtr> statements;
    int line;
};

struct If {
    ExprPtr condition;
    StmtPtr then_branch;
    StmtPtr else_branch;  // null when there is no else
    int line;
};

struct While {
    ExprPtr condition;
    StmtPtr body;
    int line;
};

struct Function {
    std::string_view name;
    std::vector<std::string_view> params;
    Block body;
    int line;
};

struct Return {
    ExprPtr value;  // null for a bare `return;`
    int line;
};

struct Stmt {
    using Node = std::variant<Print, ExprStmt, Let, Block, If, While, Function, Return>;
    Node node;
    Stmt(Node n) : node(std::move(n)) {}
};

// Owns everything the tree points into: the source text (names are views into it) and the
// tokens. Held by unique_ptr and never moved, so those addresses stay put (a moved std::string
// with a short buffer would invalidate every string_view).
struct Program {
    std::string source;
    std::vector<Token> tokens;
    std::vector<StmtPtr> statements;

    Program() = default;
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
};

}  // namespace rung
