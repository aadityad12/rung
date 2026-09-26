#include "ast_dump.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace rung {
namespace {

// Format of --dump-ast. Every node is "(head child ...)"; leaves are bare.
//
//   literals      1  2.5  "text"  true  false  nil
//   variable      x   (after the resolver: x@global, or x@N for a local N scopes up)
//   unary         (- x)  (! x)
//   binary        (+ a b)  (== a b)  ... all of + - * / % == != < <= > >=
//   logical       (and a b)  (or a b)
//   assignment    (set x v)  (set x@global v)  (set-index a i v)
//   call, index   (call f 1 2)  (index a 0)
//   array         (array 1 2)
//   statements    (print e)  (expr e)  (let x)  (let x e)  (return)  (return e)
//                 (block s ...)  (if c then)  (if c then else)  (while c body)
//                 (fn name (p1 p2) (block s ...))
//
// A `for` loop has no node of its own; it shows up as its desugared block and while.

// A variable use's name, with its resolver binding once there is one (notes D11):
// `x@global`, or `x@N` for a local declared N scopes up. Unresolved trees print the bare name,
// which is what the parser tests see.
std::string use_name(std::string_view name, const Binding& binding) {
    std::string out(name);
    if (binding.kind == BindingKind::Global) out += "@global";
    if (binding.kind == BindingKind::Local) out += "@" + std::to_string(binding.hops);
    return out;
}

std::string quote(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default: out += c;
        }
    }
    return out + '"';
}

// Shortest %g form that reads back exactly, always showing it is a float. This is for display
// in the dump only; program output uses the runtime's printer (notes §2.3).
std::string float_text(double value) {
    char buffer[40];
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buffer, sizeof buffer, "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value) break;
    }
    std::string text = buffer;
    if (text.find_first_of(".e") == std::string::npos) text += ".0";
    return text;
}

const char* binary_name(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
        case BinaryOp::Eq: return "==";
        case BinaryOp::Ne: return "!=";
        case BinaryOp::Lt: return "<";
        case BinaryOp::Le: return "<=";
        case BinaryOp::Gt: return ">";
        case BinaryOp::Ge: return ">=";
    }
    return "?";
}

std::string dump_expr(const Expr& expr);
std::string dump_stmt(const Stmt& stmt);

std::string join(std::string head, const std::vector<ExprPtr>& items) {
    for (const ExprPtr& item : items) head += " " + dump_expr(*item);
    return head + ")";
}

// A node that starts with a child expression (a binary operator's left side, a call's callee, an
// indexed object) dumps as head + <that child> + tail. A chain like `1 + 1 + ... + 1` or
// `f()()()` nests these to any length: the nesting limit (notes §2.6) does not bound it, so
// dump_expr walks the chain with a loop instead of recursing, and appends heads then tails.
std::string chain_head(const Binary& n) { return std::string("(") + binary_name(n.op) + " "; }
std::string chain_head(const Logical& n) {
    return std::string("(") + (n.op == LogicalOp::And ? "and" : "or") + " ";
}
std::string chain_head(const Call&) { return "(call "; }
std::string chain_head(const Index&) { return "(index "; }
std::string chain_head(const IndexAssign&) { return "(set-index "; }

std::string chain_tail(const Binary& n) { return " " + dump_expr(*n.right) + ")"; }
std::string chain_tail(const Logical& n) { return " " + dump_expr(*n.right) + ")"; }
std::string chain_tail(const Call& n) {
    std::string out;
    for (const ExprPtr& arg : n.args) out += " " + dump_expr(*arg);
    return out + ")";
}
std::string chain_tail(const Index& n) { return " " + dump_expr(*n.index) + ")"; }
std::string chain_tail(const IndexAssign& n) {
    return " " + dump_expr(*n.index) + " " + dump_expr(*n.value) + ")";
}

struct ExprDumper {
    std::string operator()(const Literal& n) const {
        struct V {
            std::string operator()(std::monostate) const { return "nil"; }
            std::string operator()(bool b) const { return b ? "true" : "false"; }
            std::string operator()(std::int32_t i) const { return std::to_string(i); }
            std::string operator()(double d) const { return float_text(d); }
            std::string operator()(const std::string& s) const { return quote(s); }
        };
        return std::visit(V{}, n.value);
    }
    std::string operator()(const Variable& n) const { return use_name(n.name, n.binding); }
    std::string operator()(const Assign& n) const {
        return "(set " + use_name(n.name, n.binding) + " " + dump_expr(*n.value) + ")";
    }
    std::string operator()(const Unary& n) const {
        return std::string("(") + (n.op == UnaryOp::Not ? "!" : "-") + " " +
               dump_expr(*n.operand) + ")";
    }
    std::string operator()(const ArrayLiteral& n) const { return join("(array", n.elements); }

    // Nodes whose dump starts with a child (see leftmost_child). dump_expr handles these through
    // head/tail; these overloads are the same text for a node dumped on its own.
    std::string operator()(const Binary& n) const {
        return chain_head(n) + dump_expr(*n.left) + chain_tail(n);
    }
    std::string operator()(const Logical& n) const {
        return chain_head(n) + dump_expr(*n.left) + chain_tail(n);
    }
    std::string operator()(const Call& n) const {
        return chain_head(n) + dump_expr(*n.callee) + chain_tail(n);
    }
    std::string operator()(const Index& n) const {
        return chain_head(n) + dump_expr(*n.object) + chain_tail(n);
    }
    std::string operator()(const IndexAssign& n) const {
        return chain_head(n) + dump_expr(*n.object) + chain_tail(n);
    }
};

std::string dump_block(const Block& block) {
    std::string out = "(block";
    for (const StmtPtr& stmt : block.statements) out += " " + dump_stmt(*stmt);
    return out + ")";
}

struct StmtDumper {
    std::string operator()(const Print& n) const { return "(print " + dump_expr(*n.value) + ")"; }
    std::string operator()(const ExprStmt& n) const { return "(expr " + dump_expr(*n.expr) + ")"; }
    std::string operator()(const Let& n) const {
        std::string out = "(let " + std::string(n.name);
        if (n.init) out += " " + dump_expr(*n.init);
        return out + ")";
    }
    std::string operator()(const Block& n) const { return dump_block(n); }
    std::string operator()(const If& n) const {
        std::string out = "(if " + dump_expr(*n.condition) + " " + dump_stmt(*n.then_branch);
        if (n.else_branch) out += " " + dump_stmt(*n.else_branch);
        return out + ")";
    }
    std::string operator()(const While& n) const {
        return "(while " + dump_expr(*n.condition) + " " + dump_stmt(*n.body) + ")";
    }
    std::string operator()(const Function& n) const {
        std::string out = "(fn " + std::string(n.name) + " (";
        for (std::size_t i = 0; i < n.params.size(); ++i) {
            if (i > 0) out += " ";
            out += n.params[i];
        }
        return out + ") " + dump_block(n.body) + ")";
    }
    std::string operator()(const Return& n) const {
        return n.value ? "(return " + dump_expr(*n.value) + ")" : "(return)";
    }
};

// The child a node's dump starts with, or null.
const Expr* leftmost_child(const Expr& expr) {
    return std::visit(
        [](const auto& n) -> const Expr* {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Binary> || std::is_same_v<T, Logical>) {
                return n.left.get();
            } else if constexpr (std::is_same_v<T, Call>) {
                return n.callee.get();
            } else if constexpr (std::is_same_v<T, Index> || std::is_same_v<T, IndexAssign>) {
                return n.object.get();
            } else {
                return nullptr;
            }
        },
        expr.node);
}

std::string head_of(const Expr& expr) {
    return std::visit(
        [](const auto& n) -> std::string {
            if constexpr (requires { chain_head(n); }) return chain_head(n);
            return {};  // not a chain node; never on the spine
        },
        expr.node);
}

std::string tail_of(const Expr& expr) {
    return std::visit(
        [](const auto& n) -> std::string {
            if constexpr (requires { chain_tail(n); }) return chain_tail(n);
            return {};
        },
        expr.node);
}

std::string dump_expr(const Expr& expr) {
    std::vector<const Expr*> spine;  // outermost first
    const Expr* innermost = &expr;
    while (const Expr* child = leftmost_child(*innermost)) {
        spine.push_back(innermost);
        innermost = child;
    }
    std::string out;
    for (const Expr* node : spine) out += head_of(*node);
    out += std::visit(ExprDumper{}, innermost->node);
    for (auto it = spine.rbegin(); it != spine.rend(); ++it) out += tail_of(**it);
    return out;
}
std::string dump_stmt(const Stmt& stmt) { return std::visit(StmtDumper{}, stmt.node); }

}  // namespace

std::string dump_ast(const Program& program) {
    std::string out;
    for (const StmtPtr& stmt : program.statements) out += dump_stmt(*stmt) + "\n";
    return out;
}

}  // namespace rung
