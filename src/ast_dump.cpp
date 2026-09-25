#include "ast_dump.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <variant>

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
    std::string operator()(const Binary& n) const {
        return std::string("(") + binary_name(n.op) + " " + dump_expr(*n.left) + " " +
               dump_expr(*n.right) + ")";
    }
    std::string operator()(const Logical& n) const {
        return std::string("(") + (n.op == LogicalOp::And ? "and" : "or") + " " +
               dump_expr(*n.left) + " " + dump_expr(*n.right) + ")";
    }
    std::string operator()(const Call& n) const {
        return join("(call " + dump_expr(*n.callee), n.args);
    }
    std::string operator()(const ArrayLiteral& n) const { return join("(array", n.elements); }
    std::string operator()(const Index& n) const {
        return "(index " + dump_expr(*n.object) + " " + dump_expr(*n.index) + ")";
    }
    std::string operator()(const IndexAssign& n) const {
        return "(set-index " + dump_expr(*n.object) + " " + dump_expr(*n.index) + " " +
               dump_expr(*n.value) + ")";
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

std::string dump_expr(const Expr& expr) { return std::visit(ExprDumper{}, expr.node); }
std::string dump_stmt(const Stmt& stmt) { return std::visit(StmtDumper{}, stmt.node); }

}  // namespace

std::string dump_ast(const Program& program) {
    std::string out;
    for (const StmtPtr& stmt : program.statements) out += dump_stmt(*stmt) + "\n";
    return out;
}

}  // namespace rung
