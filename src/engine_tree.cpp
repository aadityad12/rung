#include "engine_tree.h"

#include <cassert>
#include <string>
#include <utility>
#include <variant>

#include "runtime/natives.h"
#include "runtime/ops.h"

namespace rung {

namespace {

// std::visit needs one callable; this builds it from lambdas.
template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

}  // namespace

TreeEngine::TreeEngine(Heap& heap, Output& out) : heap_(heap), out_(out) {
    // The root marker goes in first: register_natives allocates, and the natives must already be
    // reachable through globals_ by the time the next allocation collects.
    root_handle_ = heap_.add_root_marker([this](Heap& h) { mark_roots(h); });
    register_natives(heap_, [this](std::string_view name, Value native) {
        globals_[name_of(name)] = native;
    });
}

TreeEngine::~TreeEngine() { heap_.remove_root_marker(root_handle_); }

// What the collector must keep alive (notes D10): the globals, every environment and function
// of the calls and blocks running right now, values still being assembled (call arguments,
// array elements), a return value in flight, and the interned names of the syntax tree (they
// are the keys of every scope map, and the intern table is weak).
void TreeEngine::mark_roots(Heap& heap) {
    for (auto& [name, value] : globals_) {
        heap.mark_object(name);
        heap.mark_value(value);
    }
    for (auto& entry : names_) heap.mark_object(entry.second);
    for (Obj* obj : active_) heap.mark_object(obj);
    for (Value v : stack_) heap.mark_value(v);
    heap.mark_value(return_value_);
}

ObjString* TreeEngine::name_of(std::string_view name) {
    auto it = names_.find(name);
    if (it != names_.end()) return it->second;
    ObjString* interned = heap_.intern(name);
    // Keyed by the string's own bytes, not the caller's view, so the key never dangles.
    names_.emplace(std::string_view(interned->chars), interned);
    return interned;
}

Value TreeEngine::fail(int line, std::string message) {
    error_ = RuntimeError{line, std::move(message)};
    return make_nil();
}

std::optional<RuntimeError> TreeEngine::take_error() {
    std::optional<RuntimeError> error = std::move(error_);
    error_.reset();
    return error;
}

// ---- entry points ----------------------------------------------------------------------------

EngineResult TreeEngine::run(const Program& program) {
    EngineResult result;
    for (const StmtPtr& stmt : program.statements) {
        // The resolver rejects top-level `return`, so the flow is Normal or Error here.
        if (exec(*stmt, nullptr) == Flow::Error) break;
    }
    result.runtime_error = take_error();
    return result;
}

CallResult TreeEngine::call_global(std::string_view name) {
    CallResult result;
    // Interned, not cached in names_: the caller's view may not outlive this call. Nothing
    // allocates between here and the lookup, so the string cannot be collected.
    ObjString* key = heap_.intern(name);
    auto it = globals_.find(key);
    if (it == globals_.end()) {
        result.runtime_error = RuntimeError{0, undefined_variable_message(name)};
        return result;
    }
    Value callee = it->second;
    Heap::TempRoot keep_callee(heap_, callee);
    StackRegion args(*this);  // no arguments, so nothing is pushed
    result.value = call_value(callee, args.base(), 0, 0);
    result.runtime_error = take_error();
    return result;
}

// ---- statements ------------------------------------------------------------------------------

TreeEngine::Flow TreeEngine::exec(const Stmt& stmt, Env env) {
    return std::visit([&](const auto& node) { return exec_node(node, env); }, stmt.node);
}

TreeEngine::Flow TreeEngine::exec_node(const Print& n, Env env) {
    Value value = eval(*n.value, env);
    if (failed()) return Flow::Error;
    std::string text;
    print_value(value, text);
    text += '\n';
    out_.write(text);
    return Flow::Normal;
}

TreeEngine::Flow TreeEngine::exec_node(const ExprStmt& n, Env env) {
    eval(*n.expr, env);
    return failed() ? Flow::Error : Flow::Normal;
}

TreeEngine::Flow TreeEngine::exec_node(const Let& n, Env env) {
    // Intern the name before evaluating: name_of can allocate, and the value would be held only
    // in a C++ local while it did.
    ObjString* name = name_of(n.name);
    Value value = n.init ? eval(*n.init, env) : make_nil();
    if (failed()) return Flow::Error;
    define(env, name, value);
    return Flow::Normal;
}

TreeEngine::Flow TreeEngine::exec_node(const Block& n, Env env) {
    // Always a new scope, even if the block declares nothing: the resolver counted this block
    // as one scope when it computed every `hops` inside it.
    ObjEnvironment* scope = heap_.allocate<ObjEnvironment>(env);
    ActiveRoot keep_scope(*this, scope);
    for (const StmtPtr& stmt : n.statements) {
        Flow flow = exec(*stmt, scope);
        if (flow != Flow::Normal) return flow;
    }
    return Flow::Normal;
}

TreeEngine::Flow TreeEngine::exec_node(const If& n, Env env) {
    Value condition = eval(*n.condition, env);
    if (failed()) return Flow::Error;
    if (is_truthy(condition)) return exec(*n.then_branch, env);
    if (n.else_branch) return exec(*n.else_branch, env);
    return Flow::Normal;
}

TreeEngine::Flow TreeEngine::exec_node(const While& n, Env env) {
    while (true) {
        Value condition = eval(*n.condition, env);
        if (failed()) return Flow::Error;
        if (!is_truthy(condition)) return Flow::Normal;
        Flow flow = exec(*n.body, env);
        if (flow != Flow::Normal) return flow;
    }
}

TreeEngine::Flow TreeEngine::exec_node(const Function& n, Env env) {
    ObjString* name = name_of(n.name);
    // `env` is the scope the `fn` statement runs in, and becomes the function's closure. It is
    // reachable from active_ (or is the globals), so the allocation below cannot free it.
    ObjTreeFunction* fn = heap_.allocate<ObjTreeFunction>(name, &n, env);
    define(env, name, make_obj(fn));
    return Flow::Normal;
}

TreeEngine::Flow TreeEngine::exec_node(const Return& n, Env env) {
    Value value = n.value ? eval(*n.value, env) : make_nil();
    if (failed()) return Flow::Error;
    return_value_ = value;
    return Flow::Return;
}

// ---- expressions -----------------------------------------------------------------------------
// Every eval below checks failed() after each sub-evaluation and returns nil at once if a
// runtime error was recorded, so the error travels up as ordinary returns (see Flow).

Value TreeEngine::eval(const Expr& expr, Env env) {
    return std::visit([&](const auto& node) { return eval_node(node, env); }, expr.node);
}

Value TreeEngine::eval_node(const Literal& n, Env) {
    return std::visit(Overloaded{
                          [](std::monostate) { return make_nil(); },
                          [](bool b) { return make_bool(b); },
                          [](std::int32_t i) { return make_int(i); },
                          [](double d) { return make_float(d); },
                          [this](const std::string& s) {
                              return make_obj(heap_.intern(std::string_view(s)));
                          },
                      },
                      n.value);
}

Value TreeEngine::eval_node(const Variable& n, Env env) {
    ObjString* name = name_of(n.name);
    Value* slot = lookup(env, n.binding, name);
    if (slot == nullptr) return fail(n.line, undefined_variable_message(n.name));
    return *slot;
}

Value TreeEngine::eval_node(const Assign& n, Env env) {
    ObjString* name = name_of(n.name);
    Value value = eval(*n.value, env);
    if (failed()) return make_nil();
    // The value is still only in a C++ local, but nothing below allocates.
    Value* slot = lookup(env, n.binding, name);
    if (slot == nullptr) return fail(n.line, undefined_variable_message(n.name));
    *slot = value;
    return value;
}

Value TreeEngine::eval_node(const Unary& n, Env env) {
    Value operand = eval(*n.operand, env);
    if (failed()) return make_nil();
    if (n.op == UnaryOp::Not) return op_not(operand);
    Value result;
    std::string error;
    if (!op_negate(operand, &result, &error)) return fail(n.line, std::move(error));
    return result;
}

Value TreeEngine::eval_node(const Binary& n, Env env) {
    Value left = eval(*n.left, env);
    if (failed()) return make_nil();
    // Evaluating the right side can allocate (and collect), and `left` lives only in this local.
    Heap::TempRoot keep_left(heap_, left);
    Value right = eval(*n.right, env);
    if (failed()) return make_nil();
    Heap::TempRoot keep_right(heap_, right);

    if (n.op == BinaryOp::Eq) return make_bool(values_equal(left, right));
    if (n.op == BinaryOp::Ne) return make_bool(!values_equal(left, right));

    Value result;
    std::string error;
    bool ok = false;
    switch (n.op) {
        case BinaryOp::Add: ok = op_add(heap_, left, right, &result, &error); break;
        case BinaryOp::Sub: ok = op_sub(left, right, &result, &error); break;
        case BinaryOp::Mul: ok = op_mul(left, right, &result, &error); break;
        case BinaryOp::Div: ok = op_div(left, right, &result, &error); break;
        case BinaryOp::Mod: ok = op_mod(left, right, &result, &error); break;
        case BinaryOp::Lt: ok = op_less(left, right, &result, &error); break;
        case BinaryOp::Le: ok = op_less_equal(left, right, &result, &error); break;
        case BinaryOp::Gt: ok = op_greater(left, right, &result, &error); break;
        case BinaryOp::Ge: ok = op_greater_equal(left, right, &result, &error); break;
        case BinaryOp::Eq:
        case BinaryOp::Ne:
            break;  // handled above
    }
    if (!ok) return fail(n.line, std::move(error));
    return result;
}

Value TreeEngine::eval_node(const Logical& n, Env env) {
    // The deciding operand's value, not a forced boolean (notes §2.2).
    Value left = eval(*n.left, env);
    if (failed()) return make_nil();
    bool left_truthy = is_truthy(left);
    if (n.op == LogicalOp::And ? !left_truthy : left_truthy) return left;
    return eval(*n.right, env);
}

Value TreeEngine::eval_node(const Call& n, Env env) {
    Value callee = eval(*n.callee, env);
    if (failed()) return make_nil();
    // Arguments are evaluated while `callee` is held only in this local.
    Heap::TempRoot keep_callee(heap_, callee);
    StackRegion args(*this);
    for (const ExprPtr& arg : n.args) {
        Value value = eval(*arg, env);
        if (failed()) return make_nil();
        stack_.push_back(value);  // from here the marker sees it through stack_
    }
    return call_value(callee, args.base(), n.args.size(), n.line);
}

Value TreeEngine::eval_node(const ArrayLiteral& n, Env env) {
    StackRegion elements(*this);
    for (const ExprPtr& element : n.elements) {
        Value value = eval(*element, env);
        if (failed()) return make_nil();
        stack_.push_back(value);
    }
    // The elements stay on stack_ (rooted) while the array is allocated, and are only then
    // released by `elements` going out of scope.
    std::vector<Value> copy(stack_.begin() + static_cast<std::ptrdiff_t>(elements.base()),
                            stack_.end());
    return make_obj(heap_.allocate<ObjArray>(std::move(copy)));
}

Value TreeEngine::eval_node(const Index& n, Env env) {
    Value object = eval(*n.object, env);
    if (failed()) return make_nil();
    Heap::TempRoot keep_object(heap_, object);
    Value index = eval(*n.index, env);
    if (failed()) return make_nil();
    Value result;
    std::string error;
    if (!array_get(object, index, &result, &error)) return fail(n.line, std::move(error));
    return result;
}

Value TreeEngine::eval_node(const IndexAssign& n, Env env) {
    Value object = eval(*n.object, env);
    if (failed()) return make_nil();
    Heap::TempRoot keep_object(heap_, object);
    Value index = eval(*n.index, env);
    if (failed()) return make_nil();
    Heap::TempRoot keep_index(heap_, index);
    Value value = eval(*n.value, env);
    if (failed()) return make_nil();
    std::string error;
    if (!array_set(object, index, value, &error)) return fail(n.line, std::move(error));
    return value;
}

// ---- variables -------------------------------------------------------------------------------

void TreeEngine::define(Env env, ObjString* name, Value value) {
    // Top-level `let` and `fn` are globals and may be redeclared; anywhere else the resolver has
    // already rejected a second declaration in the same scope (notes D11).
    if (env == nullptr) {
        globals_[name] = value;
    } else {
        env->vars[name] = value;
    }
}

// The storage for a variable use, or null for an undefined global. A local is always found: the
// resolver only binds a use to a declaration that comes before it in the same or an enclosing
// scope, and a scope's statements run in order.
Value* TreeEngine::lookup(Env env, const Binding& binding, ObjString* name) {
    assert(binding.kind != BindingKind::Unresolved && "the resolver must run before an engine");
    if (binding.kind == BindingKind::Global) {
        auto it = globals_.find(name);
        return it == globals_.end() ? nullptr : &it->second;
    }
    for (int i = 0; i < binding.hops; ++i) env = env->enclosing;  // never a search up the chain
    auto it = env->vars.find(name);
    assert(it != env->vars.end());
    return &it->second;
}

// ---- calls -----------------------------------------------------------------------------------

// Arguments are stack_[base, base + argc). `callee` is rooted by the caller.
Value TreeEngine::call_value(Value callee, std::size_t base, std::size_t argc, int line) {
    if (is_tree_function(callee)) return call_function(as_tree_function(callee), base, argc, line);
    if (is_native(callee)) {
        ObjNative* native = as_native(callee);
        if (static_cast<std::size_t>(native->arity) != argc) {
            return fail(line, arity_error_message(native->arity, static_cast<int>(argc)));
        }
        Value result;
        std::string error;
        if (!native->function(heap_, stack_.data() + base, &result, &error)) {
            return fail(line, std::move(error));
        }
        return result;
    }
    return fail(line, kErrNotCallable);
}

Value TreeEngine::call_function(ObjTreeFunction* fn, std::size_t base, std::size_t argc,
                                int line) {
    const Function& decl = *fn->declaration;
    if (decl.params.size() != argc) {
        return fail(line, arity_error_message(static_cast<int>(decl.params.size()),
                                              static_cast<int>(argc)));
    }
    if (depth_ >= kMaxCallDepth) return fail(line, kErrStackOverflow);

    // The call's scope is created inside the function's closure, so a name not found here is
    // found by following the resolver's hops outward through the scopes the function was
    // declared in. The arguments are still rooted on stack_ while it is allocated.
    ObjEnvironment* scope = heap_.allocate<ObjEnvironment>(fn->closure);
    ActiveRoot keep_function(*this, fn);
    ActiveRoot keep_scope(*this, scope);
    for (std::size_t i = 0; i < decl.params.size(); ++i) {
        ObjString* name = name_of(decl.params[i]);
        scope->vars[name] = stack_[base + i];
    }

    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
    } depth_guard(depth_);

    // The parameters and the body's own statements share one scope (notes §2.7), so the body is
    // run in `scope` directly, not through exec_node(Block).
    for (const StmtPtr& stmt : decl.body.statements) {
        Flow flow = exec(*stmt, scope);
        if (flow == Flow::Error) return make_nil();
        if (flow == Flow::Return) {
            Value result = return_value_;
            return_value_ = make_nil();
            return result;
        }
    }
    return make_nil();  // falling off the end
}

}  // namespace rung
