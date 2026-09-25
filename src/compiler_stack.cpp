#include "compiler_stack.h"

#include <cassert>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "bytecode/chunk.h"

// Single-pass AST -> bytecode compiler for the stack VM. Local-variable slots, scope
// tracking, upvalue resolution and jump patching follow clox (Crafting Interpreters ch. 22, 23
// and 25) closely; the differences are listed in bytecode/chunk.h and in the comments below.
// Unlike clox it walks a finished AST, not a token stream, and it never reports scope errors:
// the resolver (notes D11) already did.

namespace rung {

namespace {

class Compiler {
public:
    explicit Compiler(Heap& heap) : heap_(heap) {}

    // Throws CompileError only for functions too large for 24-bit operands.
    ObjFunction* compile_script(const Program& program);

    // The heap calls this (through the root marker) during a collection.
    void mark_roots(Heap& heap) const {
        for (const FunctionState* s = current_; s != nullptr; s = s->enclosing) {
            heap.mark_object(s->function);
        }
    }

private:
    struct Local {
        std::string_view name;
        int depth = 0;          // scope_depth where it was declared
        bool captured = false;  // some closure holds it: leaving scope must close the upvalue
    };
    struct UpvalueRef {
        std::uint8_t index;  // enclosing function's local slot, or its upvalue index
        bool is_local;
    };
    struct FunctionState {
        ObjFunction* function = nullptr;
        FunctionState* enclosing = nullptr;
        // locals[i] lives in slot i; slot 0 is the callee itself.
        std::vector<Local> locals;
        std::vector<UpvalueRef> upvalues;
        int scope_depth = 0;  // 0 is the top level of the script (globals), or nothing for fns
        // Names and string literals are interned, so pointer equality means equal bytes. One
        // pool entry per distinct string keeps a hot global's name from filling the pool.
        std::unordered_map<const ObjString*, std::size_t> string_constants;
    };

    // ---- Function setup and emission -------------------------------------------------------
    // Allocates the ObjFunction and makes `state` the current function. `state` lives on the
    // caller's C++ stack; mark_roots walks the chain of them.
    void begin_function(FunctionState& state, std::string_view name) {
        ObjFunction* fn;
        if (name.empty()) {
            fn = heap_.allocate<ObjFunction>(nullptr);
        } else {
            // allocate() may collect, and the fresh name string is only in this local.
            ObjString* interned = heap_.intern(name);
            Heap::TempRoot root(heap_, make_obj(interned));
            fn = heap_.allocate<ObjFunction>(interned);
        }
        state.function = fn;
        state.enclosing = current_;
        state.locals.push_back(Local{});  // slot 0: the function being called
        current_ = &state;
    }

    Chunk& chunk() { return current_->function->chunk; }

    void emit(OpCode op, int line) { chunk().emit_op(op, line); }
    void emit_u8(OpCode op, std::size_t operand, int line) {
        assert(operand <= 255);
        chunk().emit_op(op, line);
        chunk().emit_byte(static_cast<std::uint8_t>(operand), line);
    }
    void emit_u24(OpCode op, std::size_t operand, int line) {
        if (operand > kMaxU24) throw CompileError{line, "function too large"};
        chunk().emit_op(op, line);
        chunk().emit_u24(static_cast<std::uint32_t>(operand), line);
    }

    std::size_t constant(Value value, int line) {
        std::size_t index = chunk().add_constant(value);
        if (index > kMaxU24) throw CompileError{line, "function too large"};
        return index;
    }
    // Interns `bytes` and returns its constant-pool index. No allocation happens between the
    // intern() call and the pool holding the string, so it cannot be collected in between.
    std::size_t string_constant(std::string_view bytes, int line) {
        ObjString* str = heap_.intern(bytes);
        auto& pool = current_->string_constants;
        auto it = pool.find(str);
        if (it != pool.end()) return it->second;
        std::size_t index = constant(make_obj(str), line);
        pool.emplace(str, index);
        return index;
    }

    // A forward jump is emitted before its target exists: write the opcode with a 3-byte
    // placeholder and return where the placeholder is. patch_jump fills it in once the target
    // (the current end of the code) is known (clox's emitJump / patchJump, with 24 bits).
    std::size_t emit_jump(OpCode op, int line) {
        emit(op, line);
        std::size_t operand = chunk().code.size();
        for (int i = 0; i < 3; ++i) chunk().emit_byte(0xFF, line);
        return operand;
    }
    void patch_jump(std::size_t operand, int line) {
        // Distances count from the byte after the 3-byte operand (see OpCode::Jump).
        std::size_t distance = chunk().code.size() - (operand + 3);
        if (distance > kMaxU24) throw CompileError{line, "function too large"};
        put_u24(&chunk().code[operand], static_cast<std::uint32_t>(distance));
    }
    // A backward jump's target already exists, so there is nothing to patch.
    void emit_loop(std::size_t loop_start, int line) {
        emit(OpCode::Loop, line);
        std::size_t distance = chunk().code.size() + 3 - loop_start;
        if (distance > kMaxU24) throw CompileError{line, "function too large"};
        chunk().emit_u24(static_cast<std::uint32_t>(distance), line);
    }

    // ---- Locals and upvalues ---------------------------------------------------------------
    bool at_global_scope() const {
        return current_->enclosing == nullptr && current_->scope_depth == 0;
    }

    void add_local(std::string_view name) {
        // The resolver enforces at most 255 locals per function (notes D11); slot 0 is extra.
        // Slots 0..255 fit a one-byte operand, so the list holds at most 256 entries.
        assert(current_->locals.size() <= 255);
        current_->locals.push_back(Local{name, current_->scope_depth, false});
    }

    static int resolve_local(const FunctionState& fn, std::string_view name) {
        for (int i = static_cast<int>(fn.locals.size()) - 1; i >= 1; --i) {  // slot 0 is nameless
            if (fn.locals[static_cast<std::size_t>(i)].name == name) return i;
        }
        return -1;
    }

    static int add_upvalue(FunctionState& fn, std::uint8_t index, bool is_local) {
        for (std::size_t i = 0; i < fn.upvalues.size(); ++i) {
            if (fn.upvalues[i].index == index && fn.upvalues[i].is_local == is_local) {
                return static_cast<int>(i);
            }
        }
        // The resolver counts captured variables the same way (notes D11), so this holds.
        assert(fn.upvalues.size() < 255);
        fn.upvalues.push_back({index, is_local});
        return static_cast<int>(fn.upvalues.size()) - 1;
    }

    // Looks for `name` in the functions around `fn`. Found as a local of the immediately
    // enclosing function: capture that slot. Found further out: every function in between
    // captures it too, each from the one outside it (clox's resolveUpvalue).
    static int resolve_upvalue(FunctionState& fn, std::string_view name) {
        if (fn.enclosing == nullptr) return -1;
        int local = resolve_local(*fn.enclosing, name);
        if (local != -1) {
            fn.enclosing->locals[static_cast<std::size_t>(local)].captured = true;
            return add_upvalue(fn, static_cast<std::uint8_t>(local), true);
        }
        int upvalue = resolve_upvalue(*fn.enclosing, name);
        if (upvalue != -1) return add_upvalue(fn, static_cast<std::uint8_t>(upvalue), false);
        return -1;
    }

    // The resolver said this name is a local somewhere; find where (notes D11). Its binding
    // already ruled out globals, including the case where a local of the same name is declared
    // later in an enclosing block, so the search here can only find the right variable.
    void emit_local_access(std::string_view name, bool store, int line) {
        int slot = resolve_local(*current_, name);
        if (slot != -1) {
            emit_u8(store ? OpCode::SetLocal : OpCode::GetLocal, static_cast<std::size_t>(slot),
                    line);
            return;
        }
        int upvalue = resolve_upvalue(*current_, name);
        assert(upvalue != -1 && "resolver bound a variable to a local that is not in scope");
        emit_u8(store ? OpCode::SetUpvalue : OpCode::GetUpvalue,
                static_cast<std::size_t>(upvalue), line);
    }

    void end_scope(int line) {
        current_->scope_depth -= 1;
        // Locals of the scope that just ended are the newest ones. Leaving is a Pop, or a
        // CloseUpvalue if a closure captured it (that copies the value off the stack first).
        while (current_->locals.back().depth > current_->scope_depth) {
            emit(current_->locals.back().captured ? OpCode::CloseUpvalue : OpCode::Pop, line);
            current_->locals.pop_back();
        }
    }

    // ---- Expressions -----------------------------------------------------------------------
    void expr(const Expr& e) {
        std::visit([this](const auto& node) { compile(node); }, e.node);
    }

    void compile(const Literal& lit) {
        struct Visitor {
            Compiler& c;
            int line;
            void operator()(std::monostate) const { c.emit(OpCode::Nil, line); }
            void operator()(bool b) const { c.emit(b ? OpCode::True : OpCode::False, line); }
            void operator()(std::int32_t i) const {
                c.emit_u24(OpCode::Const, c.constant(make_int(i), line), line);
            }
            void operator()(double d) const {
                c.emit_u24(OpCode::Const, c.constant(make_float(d), line), line);
            }
            void operator()(const std::string& s) const {
                c.emit_u24(OpCode::Const, c.string_constant(s, line), line);
            }
        };
        std::visit(Visitor{*this, lit.line}, lit.value);
    }

    void compile(const Variable& var) {
        switch (var.binding.kind) {
            case BindingKind::Global:
                emit_u24(OpCode::GetGlobal, string_constant(var.name, var.line), var.line);
                return;
            case BindingKind::Local:
                emit_local_access(var.name, false, var.line);
                return;
            case BindingKind::Unresolved:
                break;
        }
        assert(false && "compile_stack needs a resolved program");
    }

    void compile(const Assign& assign) {
        expr(*assign.value);  // the assignment's value stays on the stack as its result
        switch (assign.binding.kind) {
            case BindingKind::Global:
                emit_u24(OpCode::SetGlobal, string_constant(assign.name, assign.line),
                         assign.line);
                return;
            case BindingKind::Local:
                emit_local_access(assign.name, true, assign.line);
                return;
            case BindingKind::Unresolved:
                break;
        }
        assert(false && "compile_stack needs a resolved program");
    }

    void compile(const Unary& unary) {
        expr(*unary.operand);
        emit(unary.op == UnaryOp::Negate ? OpCode::Neg : OpCode::Not, unary.line);
    }

    void compile(const Binary& binary) {
        expr(*binary.left);
        expr(*binary.right);
        OpCode op = OpCode::Add;
        switch (binary.op) {
            case BinaryOp::Add: op = OpCode::Add; break;
            case BinaryOp::Sub: op = OpCode::Sub; break;
            case BinaryOp::Mul: op = OpCode::Mul; break;
            case BinaryOp::Div: op = OpCode::Div; break;
            case BinaryOp::Mod: op = OpCode::Mod; break;
            case BinaryOp::Eq: op = OpCode::Eq; break;
            case BinaryOp::Ne: op = OpCode::Ne; break;
            case BinaryOp::Lt: op = OpCode::Lt; break;
            case BinaryOp::Le: op = OpCode::Le; break;
            case BinaryOp::Gt: op = OpCode::Gt; break;
            case BinaryOp::Ge: op = OpCode::Ge; break;
        }
        emit(op, binary.line);
    }

    // `and` / `or` keep the deciding operand's value (notes §2.2), so the jump must not pop it.
    void compile(const Logical& logical) {
        expr(*logical.left);
        if (logical.op == LogicalOp::And) {
            // Falsy left: skip the right side, leaving the left value as the result.
            std::size_t end = emit_jump(OpCode::JumpIfFalse, logical.line);
            emit(OpCode::Pop, logical.line);
            expr(*logical.right);
            patch_jump(end, logical.line);
        } else {
            // Falsy left: fall through to evaluate the right. Truthy left: jump over it.
            std::size_t evaluate_right = emit_jump(OpCode::JumpIfFalse, logical.line);
            std::size_t end = emit_jump(OpCode::Jump, logical.line);
            patch_jump(evaluate_right, logical.line);
            emit(OpCode::Pop, logical.line);
            expr(*logical.right);
            patch_jump(end, logical.line);
        }
    }

    void compile(const Call& call) {
        expr(*call.callee);
        for (const ExprPtr& arg : call.args) expr(*arg);
        emit_u8(OpCode::Call, call.args.size(), call.line);  // the resolver capped it at 255
    }

    void compile(const ArrayLiteral& array) {
        for (const ExprPtr& element : array.elements) expr(*element);
        emit_u24(OpCode::Array, array.elements.size(), array.line);
    }

    void compile(const Index& index) {
        expr(*index.object);
        expr(*index.index);
        emit(OpCode::IndexGet, index.line);
    }

    void compile(const IndexAssign& assign) {
        expr(*assign.object);
        expr(*assign.index);
        expr(*assign.value);
        emit(OpCode::IndexSet, assign.line);
    }

    // ---- Statements ------------------------------------------------------------------------
    void stmt(const Stmt& s) {
        std::visit([this](const auto& node) { compile(node); }, s.node);
    }

    void compile(const Print& print) {
        expr(*print.value);
        emit(OpCode::Print, print.line);
    }

    void compile(const ExprStmt& es) {
        expr(*es.expr);
        emit(OpCode::Pop, es.line);
    }

    void compile(const Let& let) {
        // The initializer's value ends up on top of the stack. At the top level it is moved
        // into the globals table; in a scope it simply stays there and becomes the local's slot
        // (so the local is declared only after the initializer is compiled).
        if (let.init) {
            expr(*let.init);
        } else {
            emit(OpCode::Nil, let.line);
        }
        if (at_global_scope()) {
            emit_u24(OpCode::DefineGlobal, string_constant(let.name, let.line), let.line);
        } else {
            add_local(let.name);
        }
    }

    void compile(const Block& block) {
        current_->scope_depth += 1;
        for (const StmtPtr& s : block.statements) stmt(*s);
        end_scope(block.line);
    }

    void compile(const If& branch) {
        expr(*branch.condition);
        std::size_t to_else = emit_jump(OpCode::JumpIfFalse, branch.line);
        emit(OpCode::Pop, branch.line);  // condition, on the taken path
        stmt(*branch.then_branch);
        std::size_t to_end = emit_jump(OpCode::Jump, branch.line);
        patch_jump(to_else, branch.line);
        emit(OpCode::Pop, branch.line);  // condition, on the not-taken path
        if (branch.else_branch) stmt(*branch.else_branch);
        patch_jump(to_end, branch.line);
    }

    void compile(const While& loop) {
        std::size_t loop_start = chunk().code.size();
        expr(*loop.condition);
        std::size_t exit = emit_jump(OpCode::JumpIfFalse, loop.line);
        emit(OpCode::Pop, loop.line);
        stmt(*loop.body);
        emit_loop(loop_start, loop.line);
        patch_jump(exit, loop.line);
        emit(OpCode::Pop, loop.line);
    }

    void compile(const Return& ret) {
        if (ret.value) {
            expr(*ret.value);
        } else {
            emit(OpCode::Nil, ret.line);
        }
        emit(OpCode::Return, ret.line);
    }

    void compile(const Function& fn) {
        if (at_global_scope()) {
            compile_function(fn);  // leaves the closure on the stack
            emit_u24(OpCode::DefineGlobal, string_constant(fn.name, fn.line), fn.line);
        } else {
            // Declared before the body so the function can call itself (notes D11); the
            // closure the CLOSURE instruction pushes lands in exactly this slot.
            add_local(fn.name);
            compile_function(fn);
        }
    }

    // Compiles a function body into its own chunk, then emits CLOSURE in the enclosing one.
    void compile_function(const Function& fn) {
        FunctionState state;
        begin_function(state, fn.name);
        // Parameters and the body's own statements share one scope (notes §2.7).
        current_->scope_depth = 1;
        for (std::string_view param : fn.params) add_local(param);
        current_->function->arity = static_cast<int>(fn.params.size());
        for (const StmtPtr& s : fn.body.statements) stmt(*s);
        emit(OpCode::Nil, fn.line);  // falling off the end returns nil
        emit(OpCode::Return, fn.line);

        ObjFunction* done = state.function;
        done->upvalue_count = static_cast<int>(state.upvalues.size());
        current_ = state.enclosing;
        // `done` is rooted by nothing here, but nothing allocates until it is in the constants.
        emit_u24(OpCode::Closure, constant(make_obj(done), fn.line), fn.line);
        for (const UpvalueRef& up : state.upvalues) {
            chunk().emit_byte(up.is_local ? 1 : 0, fn.line);
            chunk().emit_byte(up.index, fn.line);
        }
    }

    Heap& heap_;
    FunctionState* current_ = nullptr;
};

// Removes the root marker on every exit path, including a thrown CompileError.
class RootMarkerGuard {
public:
    RootMarkerGuard(Heap& heap, Heap::RootHandle handle) : heap_(heap), handle_(handle) {}
    ~RootMarkerGuard() { heap_.remove_root_marker(handle_); }
    RootMarkerGuard(const RootMarkerGuard&) = delete;
    RootMarkerGuard& operator=(const RootMarkerGuard&) = delete;

private:
    Heap& heap_;
    Heap::RootHandle handle_;
};

}  // namespace

ObjFunction* Compiler::compile_script(const Program& program) {
    FunctionState state;
    begin_function(state, {});
    for (const StmtPtr& s : program.statements) stmt(*s);
    // The last token is Eof, whose line is where falling off the end "happens".
    int end_line = program.tokens.empty() ? 1 : program.tokens.back().line;
    emit(OpCode::Nil, end_line);
    emit(OpCode::Return, end_line);
    current_ = nullptr;
    return state.function;
}

StackCompileResult compile_stack(const Program& program, Heap& heap) {
    Compiler compiler(heap);
    RootMarkerGuard guard(
        heap, heap.add_root_marker([&compiler](Heap& h) { compiler.mark_roots(h); }));
    StackCompileResult result;
    try {
        result.function = compiler.compile_script(program);
    } catch (const CompileError& error) {
        result.error = error;
    }
    return result;
}

}  // namespace rung
