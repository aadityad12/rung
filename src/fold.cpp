#include "fold.h"

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "runtime/heap.h"
#include "runtime/ops.h"
#include "runtime/value.h"

namespace rung {
namespace {

bool is_string(const Literal& lit) { return std::holds_alternative<std::string>(lit.value); }

class Folder {
public:
    FoldStats stats;

    void statements(std::vector<StmtPtr>& list) {
        std::size_t kept = 0;
        for (std::size_t i = 0; i < list.size(); ++i) {
            statement(list[i]);
            if (!list[i]) continue;  // folded away entirely
            bool returns = std::holds_alternative<Return>(list[i]->node);
            if (kept != i) list[kept] = std::move(list[i]);
            ++kept;
            if (returns) {
                // Nothing after a `return` in its own block can run. Only the same block: a
                // return inside an `if` may not be taken, so it ends nothing.
                stats.statements_removed += list.size() - (i + 1);
                break;
            }
        }
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(kept), list.end());
    }

private:
    // ---- Literals as runtime Values ---------------------------------------------------------
    // The operations in src/runtime/ops.h work on Values, so a literal is converted, operated
    // on, and converted back. A string literal becomes a Value only to ask whether it is truthy
    // (strings never take part in a folded operation), in a private heap that outlives nothing.

    std::optional<Value> number_value(const Literal& lit) const {
        struct V {
            std::optional<Value> operator()(std::monostate) const { return make_nil(); }
            std::optional<Value> operator()(bool b) const { return make_bool(b); }
            std::optional<Value> operator()(std::int32_t i) const { return make_int(i); }
            std::optional<Value> operator()(double d) const { return make_float(d); }
            std::optional<Value> operator()(const std::string&) const { return std::nullopt; }
        };
        return std::visit(V{}, lit.value);
    }

    // Any literal as a Value. Nothing allocates between intern() and the use of the result, so
    // the string cannot be collected underneath it.
    Value any_value(const Literal& lit) {
        if (const std::string* text = std::get_if<std::string>(&lit.value)) {
            return make_obj(scratch_.intern(*text));
        }
        return *number_value(lit);
    }

    static Literal literal_of(Value v, int line) {
        if (is_nil(v)) return Literal{std::monostate{}, line};
        if (is_bool(v)) return Literal{as_bool(v), line};
        if (is_int(v)) return Literal{as_int(v), line};
        // Operations on nil, bool, int and float produce only those four kinds.
        return Literal{as_float(v), line};
    }

    static const Literal* literal(const ExprPtr& e) { return std::get_if<Literal>(&e->node); }

    void replace_with(ExprPtr& slot, Value v, int line) {
        slot = std::make_unique<Expr>(literal_of(v, line));
        ++stats.expressions_folded;
    }

    bool apply(BinaryOp op, Value a, Value b, Value* out) {
        std::string error;  // an error means "do not fold"; the message is not needed
        switch (op) {
            case BinaryOp::Add: return op_add(scratch_, a, b, out, &error);
            case BinaryOp::Sub: return op_sub(a, b, out, &error);
            case BinaryOp::Mul: return op_mul(a, b, out, &error);
            case BinaryOp::Div: return op_div(a, b, out, &error);
            case BinaryOp::Mod: return op_mod(a, b, out, &error);
            case BinaryOp::Eq: *out = make_bool(values_equal(a, b)); return true;
            case BinaryOp::Ne: *out = make_bool(!values_equal(a, b)); return true;
            case BinaryOp::Lt: return op_less(a, b, out, &error);
            case BinaryOp::Le: return op_less_equal(a, b, out, &error);
            case BinaryOp::Gt: return op_greater(a, b, out, &error);
            case BinaryOp::Ge: return op_greater_equal(a, b, out, &error);
        }
        return false;
    }

    // ---- Expressions ------------------------------------------------------------------------
    void expression(ExprPtr& slot) {
        Expr::Node& node = slot->node;
        if (auto* assign = std::get_if<Assign>(&node)) {
            expression(assign->value);
        } else if (auto* unary = std::get_if<Unary>(&node)) {
            fold_unary(slot, *unary);
        } else if (auto* binary = std::get_if<Binary>(&node)) {
            fold_binary(slot, *binary);
        } else if (auto* logical = std::get_if<Logical>(&node)) {
            fold_logical(slot, *logical);
        } else if (auto* call = std::get_if<Call>(&node)) {
            expression(call->callee);
            for (ExprPtr& arg : call->args) expression(arg);
        } else if (auto* array = std::get_if<ArrayLiteral>(&node)) {
            for (ExprPtr& element : array->elements) expression(element);
        } else if (auto* index = std::get_if<Index>(&node)) {
            expression(index->object);
            expression(index->index);
        } else if (auto* store = std::get_if<IndexAssign>(&node)) {
            expression(store->object);
            expression(store->index);
            expression(store->value);
        }
        // Literal and Variable have nothing inside.
    }

    void fold_unary(ExprPtr& slot, Unary& n) {
        expression(n.operand);
        const Literal* operand = literal(n.operand);
        if (operand == nullptr) return;
        int line = n.line;
        Value result;
        if (n.op == UnaryOp::Not) {
            result = op_not(any_value(*operand));  // never fails
        } else {
            std::optional<Value> value = number_value(*operand);
            std::string error;
            if (!value || !op_negate(*value, &result, &error)) return;  // `-"a"`: runtime error
        }
        replace_with(slot, result, line);
    }

    void fold_binary(ExprPtr& slot, Binary& n) {
        expression(n.left);
        expression(n.right);
        const Literal* left = literal(n.left);
        const Literal* right = literal(n.right);
        if (left == nullptr || right == nullptr || is_string(*left) || is_string(*right)) return;
        Value result;
        if (!apply(n.op, *number_value(*left), *number_value(*right), &result)) return;
        replace_with(slot, result, n.line);
    }

    // `lit and b` is b when lit is truthy and lit when it is not; `lit or b` the other way
    // round (notes §2.2: the deciding operand's value, not a forced bool). The literal has no
    // side effects, so skipping its evaluation changes nothing, and the operand that is not
    // chosen is never evaluated at run time either.
    void fold_logical(ExprPtr& slot, Logical& n) {
        expression(n.left);
        const Literal* left = literal(n.left);
        if (left == nullptr) {
            expression(n.right);
            return;
        }
        bool truthy = is_truthy(any_value(*left));
        bool take_right = (n.op == LogicalOp::And) == truthy;
        ExprPtr chosen = std::move(take_right ? n.right : n.left);
        if (take_right) expression(chosen);
        slot = std::move(chosen);  // destroys the Logical, and with it the dropped operand
        ++stats.expressions_folded;
    }

    // ---- Statements -------------------------------------------------------------------------
    // Folds the statement in `slot`, which may be replaced by another statement or, when
    // nothing is left of it, become null. A caller that needs a statement (an `if` or `while`
    // body) puts an empty block there; a block's statement list just drops the entry.
    void statement(StmtPtr& slot) {
        Stmt::Node& node = slot->node;
        if (auto* print = std::get_if<Print>(&node)) {
            expression(print->value);
        } else if (auto* expr_stmt = std::get_if<ExprStmt>(&node)) {
            expression(expr_stmt->expr);
        } else if (auto* let = std::get_if<Let>(&node)) {
            if (let->init) expression(let->init);
        } else if (auto* block = std::get_if<Block>(&node)) {
            statements(block->statements);
        } else if (auto* if_stmt = std::get_if<If>(&node)) {
            fold_if(slot, *if_stmt);
        } else if (auto* while_stmt = std::get_if<While>(&node)) {
            fold_while(slot, *while_stmt);
        } else if (auto* function = std::get_if<Function>(&node)) {
            statements(function->body.statements);
        } else if (auto* ret = std::get_if<Return>(&node)) {
            if (ret->value) expression(ret->value);
        }
    }

    // A body that vanished still has to be a statement.
    void nested(StmtPtr& slot, int line) {
        statement(slot);
        if (!slot) slot = std::make_unique<Stmt>(Block{{}, line});
    }

    void fold_if(StmtPtr& slot, If& n) {
        expression(n.condition);
        const Literal* condition = literal(n.condition);
        if (condition == nullptr) {
            nested(n.then_branch, n.line);
            if (n.else_branch) nested(n.else_branch, n.line);
            return;
        }
        // The branch that cannot run is dropped; the other one takes the `if`'s place. It keeps
        // its own scope if it is a block, so no variable's hops change (notes D11).
        bool taken = is_truthy(any_value(*condition));
        StmtPtr chosen = std::move(taken ? n.then_branch : n.else_branch);
        ++stats.statements_removed;
        if (chosen) statement(chosen);
        slot = std::move(chosen);  // may be null: `if (false) ...` with no else
    }

    void fold_while(StmtPtr& slot, While& n) {
        expression(n.condition);
        const Literal* condition = literal(n.condition);
        if (condition != nullptr && !is_truthy(any_value(*condition))) {
            ++stats.statements_removed;
            slot.reset();  // the body never runs and the condition has no side effects
            return;
        }
        nested(n.body, n.line);
    }

    // Used for is_truthy on string literals and for op_add's heap parameter (never allocated
    // from, because strings are not folded). It has no root markers, and nothing is kept in it
    // across a call that could collect.
    Heap scratch_;
};

}  // namespace

FoldStats fold(Program& program) {
    Folder folder;
    folder.statements(program.statements);
    return folder.stats;
}

}  // namespace rung
