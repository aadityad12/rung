#include "resolver.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace rung {
namespace {

// Thrown at the first error and caught in resolve(); the walk is recursive, so this is the
// simplest way to stop (the parser does the same).
struct ResolveAbort {
    CompileError error;
};

// One name declared in a local scope.
struct Local {
    bool defined;  // false while its own initializer is being resolved
    int id;        // unique per declaration, to count distinct captured variables
};

// A local scope: a block, or a function's parameters plus its body's statements.
struct Scope {
    std::unordered_map<std::string_view, Local> names;
    int function_level;  // index into Resolver::functions_ of the function that owns it
};

// Per-function bookkeeping. Level 0 is the top-level script, which has locals (inside blocks)
// and so gets the same limit: the VMs give it stack slots too.
struct FunctionState {
    int live_locals = 0;
    std::unordered_set<int> captured;  // ids of enclosing-function locals this function reaches
};

class Resolver {
public:
    explicit Resolver(Program& program) : program_(program) { functions_.emplace_back(); }

    void run() {
        for (StmtPtr& stmt : program_.statements) stmt_(*stmt);
    }

private:
    [[noreturn]] void fail(int line, std::string message) {
        throw ResolveAbort{CompileError{line, std::move(message)}};
    }

    // Names are views into program_.source, so a name's line is recoverable from its address.
    // The AST stores lines only for keywords and operators, but "the offending token's line"
    // (a parameter or `let` name on its own line) needs the name's own line. Runs on errors only.
    int line_of(std::string_view name) const {
        std::size_t offset = static_cast<std::size_t>(name.data() - program_.source.data());
        int line = 1;
        for (std::size_t i = 0; i < offset; ++i) {
            if (program_.source[i] == '\n') ++line;
        }
        return line;
    }

    // ---- scopes ----

    void begin_scope() { scopes_.push_back(Scope{{}, static_cast<int>(functions_.size()) - 1}); }

    void end_scope() {
        functions_.back().live_locals -= static_cast<int>(scopes_.back().names.size());
        scopes_.pop_back();
    }

    // Adds `name` to the innermost local scope, not yet defined. `line` is the name's line.
    void declare(std::string_view name, int line) {
        Scope& scope = scopes_.back();
        if (scope.names.count(name) != 0) {
            fail(line, "variable '" + std::string(name) + "' is already declared in this scope");
        }
        if (++functions_.back().live_locals > kMaxLocalLimit) {
            fail(line, "too many local variables");
        }
        scope.names.emplace(name, Local{false, next_id_++});
    }

    void define(std::string_view name) { scopes_.back().names.at(name).defined = true; }

    // Records how a use of `name` binds: the innermost scope that declares it, else global.
    void bind(std::string_view name, int line, Binding& binding) {
        for (int i = static_cast<int>(scopes_.size()) - 1; i >= 0; --i) {
            Scope& scope = scopes_[static_cast<std::size_t>(i)];
            auto found = scope.names.find(name);
            if (found == scope.names.end()) continue;
            if (!found->second.defined) {
                fail(line, "can't read local variable '" + std::string(name) +
                               "' in its own initializer");
            }
            binding = Binding{BindingKind::Local, static_cast<int>(scopes_.size()) - 1 - i};
            note_capture(scope.function_level, found->second.id, line);
            return;
        }
        binding = Binding{BindingKind::Global, 0};
    }

    // A variable declared in function `owner` but used in a deeper function is captured by
    // that function and by every function in between, which must carry it through (this is
    // how clox counts upvalues). A set per function counts each variable once.
    void note_capture(int owner, int id, int line) {
        int current = static_cast<int>(functions_.size()) - 1;
        for (int level = owner + 1; level <= current; ++level) {
            auto& captured = functions_[static_cast<std::size_t>(level)].captured;
            captured.insert(id);
            if (static_cast<int>(captured.size()) > kMaxLocalLimit) {
                fail(line, "too many captured variables");
            }
        }
    }

    // ---- statements ----

    void stmt_(Stmt& stmt) {
        std::visit([this](auto& node) { visit(node); }, stmt.node);
    }

    void visit(Print& n) { expr_(*n.value); }
    void visit(ExprStmt& n) { expr_(*n.expr); }

    void visit(Let& n) {
        if (scopes_.empty()) {  // a global: may be redeclared, and has no slot to conflict with
            if (n.init) expr_(*n.init);
            return;
        }
        // Declare, resolve the initializer, then define, so `let a = a;` in a block cannot
        // read the half-made local (notes D11).
        declare(n.name, line_of(n.name));
        if (n.init) expr_(*n.init);
        define(n.name);
    }

    void visit(Block& n) {
        begin_scope();
        for (StmtPtr& stmt : n.statements) stmt_(*stmt);
        end_scope();
    }

    void visit(If& n) {
        expr_(*n.condition);
        stmt_(*n.then_branch);
        if (n.else_branch) stmt_(*n.else_branch);
    }

    void visit(While& n) {
        expr_(*n.condition);
        stmt_(*n.body);
    }

    void visit(Function& n) {
        // The name is defined before the body is resolved so the function can recurse.
        if (!scopes_.empty()) {
            declare(n.name, line_of(n.name));
            define(n.name);
        }
        functions_.emplace_back();
        // One scope holds the parameters and the body's own statements (the body Block does not
        // open a second scope), so `fn f(a) { let a; }` is a redeclaration, as in clox.
        begin_scope();
        for (std::size_t i = 0; i < n.params.size(); ++i) {
            if (i >= static_cast<std::size_t>(kMaxLocalLimit)) {
                fail(line_of(n.params[i]), "too many parameters");
            }
            declare(n.params[i], line_of(n.params[i]));
            define(n.params[i]);
        }
        for (StmtPtr& stmt : n.body.statements) stmt_(*stmt);
        end_scope();
        functions_.pop_back();
    }

    void visit(Return& n) {
        if (functions_.size() == 1) fail(n.line, "can't return from top-level code");
        if (n.value) expr_(*n.value);
    }

    // ---- expressions ----

    void expr_(Expr& expr) {
        std::visit([this](auto& node) { visit(node); }, expr.node);
    }

    void visit(Literal&) {}
    void visit(Variable& n) { bind(n.name, n.line, n.binding); }

    void visit(Assign& n) {
        expr_(*n.value);
        bind(n.name, n.line, n.binding);
    }

    void visit(Unary& n) { expr_(*n.operand); }

    void visit(Binary& n) {
        expr_(*n.left);
        expr_(*n.right);
    }

    void visit(Logical& n) {
        expr_(*n.left);
        expr_(*n.right);
    }

    void visit(Call& n) {
        expr_(*n.callee);
        for (std::size_t i = 0; i < n.args.size(); ++i) {
            if (i >= static_cast<std::size_t>(kMaxLocalLimit)) fail(n.line, "too many arguments");
            expr_(*n.args[i]);
        }
    }

    void visit(ArrayLiteral& n) {
        for (ExprPtr& element : n.elements) expr_(*element);
    }

    void visit(Index& n) {
        expr_(*n.object);
        expr_(*n.index);
    }

    void visit(IndexAssign& n) {
        expr_(*n.object);
        expr_(*n.index);
        expr_(*n.value);
    }

    Program& program_;
    std::vector<Scope> scopes_;  // local scopes only; the global scope is implicit
    std::vector<FunctionState> functions_;
    int next_id_ = 0;
};

}  // namespace

std::optional<CompileError> resolve(Program& program) {
    try {
        Resolver(program).run();
    } catch (ResolveAbort& abort) {
        return std::move(abort.error);
    }
    return std::nullopt;
}

}  // namespace rung
