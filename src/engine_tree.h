#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine.h"

namespace rung {

// Engine 1: walks the syntax tree directly, with one C++ call per node (notes D12). It is the
// deliberately slow baseline every later engine is measured against, so nothing here is
// optimised: scopes are hash maps, every variable access is a hash lookup, and every Rung call
// is several nested C++ calls.
//
// All semantics (arithmetic, comparison, printing, error text) come from src/runtime/ops.h;
// this file only decides evaluation order and manages scopes, and it says where each runtime
// error's line comes from. Environments as garbage-collected hash maps, with variable uses
// resolved to a number of hops beforehand, follow jlox in Crafting Interpreters (chapters 8 and
// 11); the rest (the GC, the natives, the error rules) is Rung's own.
class TreeEngine final : public Engine {
public:
    TreeEngine(Heap& heap, Output& out);
    ~TreeEngine() override;

    std::string_view name() const override { return "tree"; }
    EngineResult run(const Program& program) override;
    CallResult call_global(std::string_view name) override;

private:
    // The scope an expression runs in. Null means the global scope, which is the `globals_`
    // table and not an object. Every block and every call body is one ObjEnvironment, matching
    // the resolver's scopes one to one, so `Binding::hops` counts these links (notes D11).
    using Env = ObjEnvironment*;

    // How a statement ended. Neither `return` nor a runtime error is a C++ exception: `return`
    // is the ordinary way out of most calls, and an exception costs far more than a returned
    // status. An error is recorded in `error_`, and every evaluation checks failed() and passes
    // it up, because throwing from 10,000 nested Rung calls unwinds a native stack that
    // AddressSanitizer cannot cope with (notes §5).
    enum class Flow { Normal, Return, Error };

    // Keeps an object visible to the collector while a C++ scope is alive.
    class ActiveRoot {
    public:
        ActiveRoot(TreeEngine& engine, Obj* obj) : engine_(engine) {
            engine_.active_.push_back(obj);
        }
        ~ActiveRoot() { engine_.active_.pop_back(); }
        ActiveRoot(const ActiveRoot&) = delete;
        ActiveRoot& operator=(const ActiveRoot&) = delete;

    private:
        TreeEngine& engine_;
    };

    // Values pushed on `stack_` since construction are released when it goes out of scope, even
    // if a runtime error unwinds through it.
    class StackRegion {
    public:
        explicit StackRegion(TreeEngine& engine) : engine_(engine), base_(engine.stack_.size()) {}
        ~StackRegion() { engine_.stack_.resize(base_); }
        StackRegion(const StackRegion&) = delete;
        StackRegion& operator=(const StackRegion&) = delete;
        std::size_t base() const { return base_; }

    private:
        TreeEngine& engine_;
        std::size_t base_;
    };

    // ---- statements ----
    Flow exec(const Stmt& stmt, Env env);
    Flow exec_node(const Print& n, Env env);
    Flow exec_node(const ExprStmt& n, Env env);
    Flow exec_node(const Let& n, Env env);
    Flow exec_node(const Block& n, Env env);
    Flow exec_node(const If& n, Env env);
    Flow exec_node(const While& n, Env env);
    Flow exec_node(const Function& n, Env env);
    Flow exec_node(const Return& n, Env env);

    // ---- expressions ----
    Value eval(const Expr& expr, Env env);
    Value eval_node(const Literal& n, Env env);
    Value eval_node(const Variable& n, Env env);
    Value eval_node(const Assign& n, Env env);
    Value eval_node(const Unary& n, Env env);
    Value eval_node(const Binary& n, Env env);
    Value eval_node(const Logical& n, Env env);
    Value eval_node(const Call& n, Env env);
    Value eval_node(const ArrayLiteral& n, Env env);
    Value eval_node(const Index& n, Env env);
    Value eval_node(const IndexAssign& n, Env env);

    // ---- helpers ----
    // The interned string for a name in the syntax tree. Cached (and kept alive) in `names_`, so
    // a name always maps to the same ObjString and the scope maps can be keyed by pointer.
    ObjString* name_of(std::string_view name);
    bool failed() const { return error_.has_value(); }
    // Records the run's runtime error and returns nil, which every caller passes straight up.
    Value fail(int line, std::string message);
    std::optional<RuntimeError> take_error();
    void define(Env env, ObjString* name, Value value);
    Value* lookup(Env env, const Binding& binding, ObjString* name);
    Value call_value(Value callee, std::size_t base, std::size_t argc, int line);
    Value call_function(ObjTreeFunction* fn, std::size_t base, std::size_t argc, int line);
    void mark_roots(Heap& heap);

    Heap& heap_;
    Output& out_;
    Heap::RootHandle root_handle_;

    std::unordered_map<ObjString*, Value> globals_;
    std::unordered_map<std::string_view, ObjString*> names_;
    std::vector<Obj*> active_;  // environments and functions of the calls and blocks running now
    std::vector<Value> stack_;  // call arguments and array elements evaluated so far
    Value return_value_ = make_nil();
    std::optional<RuntimeError> error_;  // set once a runtime error has happened
    int depth_ = 0;  // active Rung calls (notes §2.4)
};

}  // namespace rung
