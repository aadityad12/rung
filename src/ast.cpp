#include "ast.h"

#include <variant>
#include <vector>

namespace rung {
namespace {

// Moves every child expression of `expr` onto `out`, leaving the node's own pointers null.
// Statements are not followed: their nesting is bounded by the parser (notes §2.6), and only
// expression chains can be arbitrarily long without nesting.
struct ChildDetacher {
    std::vector<ExprPtr>& out;

    void take(ExprPtr& child) {
        if (child) out.push_back(std::move(child));
    }
    void take_all(std::vector<ExprPtr>& children) {
        for (ExprPtr& child : children) take(child);
    }

    void operator()(Literal&) {}
    void operator()(Variable&) {}
    void operator()(Assign& n) { take(n.value); }
    void operator()(Unary& n) { take(n.operand); }
    void operator()(Binary& n) {
        take(n.left);
        take(n.right);
    }
    void operator()(Logical& n) {
        take(n.left);
        take(n.right);
    }
    void operator()(Call& n) {
        take(n.callee);
        take_all(n.args);
    }
    void operator()(ArrayLiteral& n) { take_all(n.elements); }
    void operator()(Index& n) {
        take(n.object);
        take(n.index);
    }
    void operator()(IndexAssign& n) {
        take(n.object);
        take(n.index);
        take(n.value);
    }
};

}  // namespace

// The worklist holds subtrees that are still alive. Each popped node has its children moved onto
// the worklist before it is destroyed, so when its own destructor runs it finds nothing to
// detach and returns at once: the native stack depth stays constant however long the chain is.
Expr::~Expr() {
    std::vector<ExprPtr> work;
    ChildDetacher detacher{work};
    std::visit(detacher, node);
    while (!work.empty()) {
        ExprPtr next = std::move(work.back());
        work.pop_back();
        std::visit(detacher, next->node);
    }
}

}  // namespace rung
