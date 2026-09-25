#include "compiler_reg.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "bytecode/register_code.h"

// Single-pass AST -> register bytecode compiler (notes D14). Scope tracking, upvalue resolution
// and jump patching are the same as compiler_stack.cpp (clox, Crafting Interpreters ch. 22, 23
// and 25); what is new is the register allocation, which follows "The Implementation of Lua 5.0"
// (section 4): a local variable lives in a fixed register, temporaries are handed out above the
// locals in stack order, and an expression is compiled "into" a chosen destination register.
// Unlike Lua, Rung assignments are expressions and the language has no compare-and-jump
// instructions yet, so the code below has to guard against one hazard Lua does not have; see
// `operand`.

namespace rung {

namespace {

class Compiler {
public:
    explicit Compiler(Heap& heap) : heap_(heap) {}

    // Throws CompileError only for functions too large for the operand widths.
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
        std::uint8_t index;  // enclosing function's register, or its upvalue index
        bool is_local;
    };
    // An operand of an instruction that accepts RK: a register, or a constant-pool index.
    struct Operand {
        std::uint32_t index;
        bool is_const;
    };
    struct FunctionState {
        ObjFunction* function = nullptr;
        FunctionState* enclosing = nullptr;
        // locals[i] lives in register i. Registers from locals.size() up are temporaries.
        std::vector<Local> locals;
        std::vector<UpvalueRef> upvalues;
        int scope_depth = 0;  // 0 is the top level of the script (globals), or nothing for fns
        int free_reg = 0;     // the next register a temporary would get
        // One pool entry per distinct nil / bool / int / float / string, so a hot constant
        // does not fill the pool (and stays within reach of a 16-bit RK operand). Key: a type
        // tag and the value's bits; interned strings are equal exactly when their pointers are.
        std::map<std::pair<int, std::uint64_t>, std::size_t> constants;
    };

    // ---- Function setup and emission -------------------------------------------------------
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
        current_ = &state;
    }

    RegChunk& code() { return current_->function->reg; }
    std::size_t here() { return code().code.size(); }

    void emit(Instruction insn, int line) { code().emit(insn, line); }
    void emit_abc(RegOp op, std::uint32_t a, std::uint32_t b, std::uint32_t c, int line) {
        emit(make_abc(op, a, b, c), line);
    }
    // For instructions with RK operands: sets the constant flags from the operands.
    void emit_rk(RegOp op, std::uint32_t a, Operand b, Operand c, int line) {
        std::uint8_t flags = static_cast<std::uint8_t>((b.is_const ? kFlagBConst : 0) |
                                                       (c.is_const ? kFlagCConst : 0));
        emit(make_abc(op, a, b.index, c.index, flags), line);
    }
    void emit_rk_b(RegOp op, std::uint32_t a, Operand b, int line) {
        emit(make_abc(op, a, b.index, 0, b.is_const ? kFlagBConst : 0), line);
    }

    // Hands out the next temporary register and tracks the function's frame size (its
    // high-water mark). The caller frees temporaries by putting `free_reg` back.
    int reserve(int line) {
        int reg = current_->free_reg++;
        if (static_cast<std::uint32_t>(reg) > kMaxRegister) {
            throw CompileError{line, "function too large"};
        }
        int& frame = code().frame_size;
        if (current_->free_reg > frame) frame = current_->free_reg;
        return reg;
    }
    int free_reg() const { return current_->free_reg; }
    void set_free_reg(int reg) { current_->free_reg = reg; }

    // ---- Constants -------------------------------------------------------------------------
    std::size_t add_constant(int tag, std::uint64_t bits, Value value) {
        auto [it, inserted] = current_->constants.try_emplace({tag, bits}, 0);
        if (inserted) it->second = code().add_constant(value);
        return it->second;
    }
    std::size_t nil_constant() { return add_constant(0, 0, make_nil()); }
    std::size_t bool_constant(bool b) { return add_constant(1, b ? 1 : 0, make_bool(b)); }
    std::size_t int_constant(std::int32_t i) {
        return add_constant(2, static_cast<std::uint32_t>(i), make_int(i));
    }
    std::size_t float_constant(double d) {
        std::uint64_t bits;
        std::memcpy(&bits, &d, sizeof bits);  // bit pattern, so 0.0 and -0.0 stay distinct
        return add_constant(3, bits, make_float(d));
    }
    // Interns `bytes` and returns its pool index. Nothing allocates between the intern() call
    // and the pool holding the string, so it cannot be collected in between.
    std::size_t string_constant(std::string_view bytes) {
        ObjString* str = heap_.intern(bytes);
        return add_constant(4, reinterpret_cast<std::uintptr_t>(str), make_obj(str));
    }
    std::size_t literal_constant(const Literal& lit) {
        struct Visitor {
            Compiler& c;
            std::size_t operator()(std::monostate) const { return c.nil_constant(); }
            std::size_t operator()(bool b) const { return c.bool_constant(b); }
            std::size_t operator()(std::int32_t i) const { return c.int_constant(i); }
            std::size_t operator()(double d) const { return c.float_constant(d); }
            std::size_t operator()(const std::string& s) const { return c.string_constant(s); }
        };
        return std::visit(Visitor{*this}, lit.value);
    }

    // ---- Jumps -----------------------------------------------------------------------------
    // A forward jump is emitted before its target exists: write it with a placeholder offset
    // and return its index. patch_jump fills the offset in once the target (the current end of
    // the code) is known. Offsets count instructions from the one after the jump (notes D14).
    std::size_t emit_jump(RegOp op, std::uint32_t reg, int line) {
        emit(make_asbx(op, reg, 0), line);
        return here() - 1;
    }
    void patch_jump(std::size_t jump, int line) {
        std::size_t distance = here() - (jump + 1);
        if (distance > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            throw CompileError{line, "function too large"};
        }
        Instruction old = code().code[jump];
        code().code[jump] =
            make_asbx(insn_op(old), insn_a(old), static_cast<std::int32_t>(distance));
    }
    // A backward jump's target already exists, so there is nothing to patch.
    void emit_loop(std::size_t target, int line) {
        std::size_t distance = here() + 1 - target;
        if (distance > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            throw CompileError{line, "function too large"};
        }
        emit(make_asbx(RegOp::Jump, 0, -static_cast<std::int32_t>(distance)), line);
    }

    // ---- Locals and upvalues ---------------------------------------------------------------
    bool at_global_scope() const {
        return current_->enclosing == nullptr && current_->scope_depth == 0;
    }
    int local_count() const { return static_cast<int>(current_->locals.size()); }

    // A new local takes the register that is next in line, which by the statement-level
    // invariant (no temporaries live between statements) is exactly `free_reg`.
    void add_local(std::string_view name) {
        // The resolver enforces at most 255 locals per function (notes D11).
        assert(current_->locals.size() < 255);
        assert(static_cast<std::size_t>(current_->free_reg) == current_->locals.size() + 1 &&
               "declare a local right after reserving its register");
        current_->locals.push_back(Local{name, current_->scope_depth, false});
    }

    static int resolve_local(const FunctionState& fn, std::string_view name) {
        for (int i = static_cast<int>(fn.locals.size()) - 1; i >= 0; --i) {
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
    // enclosing function: capture that register. Found further out: every function in between
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

    // Where a variable the resolver bound to a local lives from the current function's point of
    // view: one of its own registers, or one of its closure's upvalues.
    struct LocalRef {
        bool is_register;
        int index;
    };
    LocalRef find_local(std::string_view name) {
        int reg = resolve_local(*current_, name);
        if (reg != -1) return {true, reg};
        int upvalue = resolve_upvalue(*current_, name);
        assert(upvalue != -1 && "resolver bound a variable to a local that is not in scope");
        return {false, upvalue};
    }

    void end_scope(int line) {
        current_->scope_depth -= 1;
        // The scope's locals are the newest ones. Upvalues that point into the scope's
        // registers must be closed (the value copied out) before the registers are reused;
        // closing from the lowest captured one closes any others above it as well.
        int first_captured = -1;
        while (!current_->locals.empty() && current_->locals.back().depth > current_->scope_depth) {
            if (current_->locals.back().captured) first_captured = local_count() - 1;
            current_->locals.pop_back();
        }
        if (first_captured != -1) {
            emit_abc(RegOp::Close, static_cast<std::uint32_t>(first_captured), 0, 0, line);
        }
        set_free_reg(local_count());
    }

    // ---- Expressions: which ones can change a register another operand is reading --------
    // True if evaluating `e` might assign to a local variable, and so change a register that an
    // already-computed operand of the same expression is reading in place. Only an assignment to
    // a local, or a call (the callee can assign to a captured local through its upvalue), can.
    static bool may_clobber_locals(const Expr& e) {
        return std::visit(
            [](const auto& node) -> bool {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, Literal> || std::is_same_v<T, Variable>) {
                    return false;
                } else if constexpr (std::is_same_v<T, Assign>) {
                    return node.binding.kind == BindingKind::Local ||
                           may_clobber_locals(*node.value);
                } else if constexpr (std::is_same_v<T, Unary>) {
                    return may_clobber_locals(*node.operand);
                } else if constexpr (std::is_same_v<T, Binary> || std::is_same_v<T, Logical>) {
                    return may_clobber_locals(*node.left) || may_clobber_locals(*node.right);
                } else if constexpr (std::is_same_v<T, Call>) {
                    return true;
                } else if constexpr (std::is_same_v<T, ArrayLiteral>) {
                    for (const ExprPtr& element : node.elements) {
                        if (may_clobber_locals(*element)) return true;
                    }
                    return false;
                } else if constexpr (std::is_same_v<T, Index>) {
                    return may_clobber_locals(*node.object) || may_clobber_locals(*node.index);
                } else {
                    static_assert(std::is_same_v<T, IndexAssign>);
                    return may_clobber_locals(*node.object) || may_clobber_locals(*node.index) ||
                           may_clobber_locals(*node.value);
                }
            },
            e.node);
    }

    // ---- Expressions -----------------------------------------------------------------------
    // Returns where the value of `e` can be read from, allocating a temporary (which the caller
    // frees by restoring free_reg) only if it must:
    //   * a literal is a constant operand, if `allow_const` and its pool index fits an RK field;
    //   * a local variable of this function is its own register, with no code at all;
    //   * anything else is computed into a fresh temporary.
    // `snapshot` says that a later operand of the same instruction may assign to a local (see
    // may_clobber_locals). Reading the register in place would then see the new value, where
    // the language (and the stack VM, which pushes a copy) says the value at the time this
    // operand was evaluated, as in `a + (a = 5)`. So a local is copied into a temporary.
    Operand operand(const Expr& e, bool allow_const, bool snapshot) {
        if (const auto* lit = std::get_if<Literal>(&e.node)) {
            if (allow_const) {
                std::size_t index = literal_constant(*lit);
                if (index <= kMaxRkConstant) return {static_cast<std::uint32_t>(index), true};
            }
        } else if (const auto* var = std::get_if<Variable>(&e.node)) {
            if (!snapshot && var->binding.kind == BindingKind::Local) {
                LocalRef ref = find_local(var->name);
                if (ref.is_register) return {static_cast<std::uint32_t>(ref.index), false};
            }
        }
        int temp = reserve(line_of(e));
        expr(e, temp);
        return {static_cast<std::uint32_t>(temp), false};
    }

    static int line_of(const Expr& e) {
        return std::visit([](const auto& node) { return node.line; }, e.node);
    }

    // Computes the value of `e` into register `dest`, which the caller has reserved. The
    // registers above free_reg are free to use as temporaries, and free_reg is the same on
    // return. `dest` is written only when the value is complete, except in the cases that
    // check dest_is_live_local() first, so `dest` may safely be a variable the expression reads.
    void expr(const Expr& e, int dest) {
        std::visit([this, dest](const auto& node) { compile(node, dest); }, e.node);
    }

    // True when `dest` is the register of a variable that is in scope. Writing it early would
    // change what the rest of the expression reads: in `a = (b + 1) and a`, storing `b + 1`
    // into a's register before evaluating the right `a` would read the wrong value.
    bool dest_is_live_local(int dest) const { return dest < local_count(); }

    // A temporary register that is the newest, so a call may put its callee there.
    bool dest_is_top_temp(int dest) const {
        return dest == free_reg() - 1 && !dest_is_live_local(dest);
    }

    void move_operand_to(int dest, Operand src, int line) {
        if (src.is_const) {
            emit(make_abx(RegOp::LoadK, static_cast<std::uint32_t>(dest),
                          static_cast<std::uint32_t>(src.index)),
                 line);
        } else if (static_cast<int>(src.index) != dest) {
            emit_abc(RegOp::Move, static_cast<std::uint32_t>(dest), src.index, 0, line);
        }
    }

    void compile(const Literal& lit, int dest) {
        auto d = static_cast<std::uint32_t>(dest);
        if (std::holds_alternative<std::monostate>(lit.value)) {
            emit_abc(RegOp::LoadNil, d, 0, 0, lit.line);
        } else if (const bool* b = std::get_if<bool>(&lit.value)) {
            emit_abc(*b ? RegOp::LoadTrue : RegOp::LoadFalse, d, 0, 0, lit.line);
        } else {
            emit(make_abx(RegOp::LoadK, d, static_cast<std::uint32_t>(literal_constant(lit))),
                 lit.line);
        }
    }

    void compile(const Variable& var, int dest) {
        auto d = static_cast<std::uint32_t>(dest);
        switch (var.binding.kind) {
            case BindingKind::Global:
                emit(make_abx(RegOp::GetGlobal, d,
                              static_cast<std::uint32_t>(string_constant(var.name))),
                     var.line);
                return;
            case BindingKind::Local: {
                LocalRef ref = find_local(var.name);
                if (ref.is_register) {
                    move_operand_to(dest, {static_cast<std::uint32_t>(ref.index), false}, var.line);
                } else {
                    emit_abc(RegOp::GetUpvalue, d, static_cast<std::uint32_t>(ref.index), 0,
                             var.line);
                }
                return;
            }
            case BindingKind::Unresolved:
                break;
        }
        assert(false && "compile_register needs a resolved program");
    }

    // An assignment is an expression whose value is the assigned value. `dest` is where that
    // value must end up, or -1 when the caller discards it (an expression statement), which
    // lets `a = b + c` on a local compile to a single ADD straight into a's register.
    void assign(const Assign& a, int dest) {
        int save = free_reg();
        switch (a.binding.kind) {
            case BindingKind::Local: {
                LocalRef ref = find_local(a.name);
                if (ref.is_register) {
                    // Compute straight into the variable's register (safe: see expr()).
                    expr(*a.value, ref.index);
                    if (dest != -1) {
                        move_operand_to(dest, {static_cast<std::uint32_t>(ref.index), false},
                                        a.line);
                    }
                    return;
                }
                int value = dest != -1 ? dest : reserve(a.line);
                expr(*a.value, value);
                emit_abc(RegOp::SetUpvalue, static_cast<std::uint32_t>(value),
                         static_cast<std::uint32_t>(ref.index), 0, a.line);
                break;
            }
            case BindingKind::Global: {
                int value = dest != -1 ? dest : reserve(a.line);
                expr(*a.value, value);
                emit(make_abx(RegOp::SetGlobal, static_cast<std::uint32_t>(value),
                              static_cast<std::uint32_t>(string_constant(a.name))),
                     a.line);
                break;
            }
            case BindingKind::Unresolved:
                assert(false && "compile_register needs a resolved program");
                break;
        }
        set_free_reg(save);
    }
    void compile(const Assign& a, int dest) { assign(a, dest); }

    void compile(const Unary& unary, int dest) {
        int save = free_reg();
        Operand value = operand(*unary.operand, true, false);
        emit_rk_b(unary.op == UnaryOp::Negate ? RegOp::Neg : RegOp::Not,
                  static_cast<std::uint32_t>(dest), value, unary.line);
        set_free_reg(save);
    }

    void compile(const Binary& binary, int dest) {
        int save = free_reg();
        Operand left = operand(*binary.left, true, may_clobber_locals(*binary.right));
        Operand right = operand(*binary.right, true, false);
        RegOp op = RegOp::Add;
        switch (binary.op) {
            case BinaryOp::Add: op = RegOp::Add; break;
            case BinaryOp::Sub: op = RegOp::Sub; break;
            case BinaryOp::Mul: op = RegOp::Mul; break;
            case BinaryOp::Div: op = RegOp::Div; break;
            case BinaryOp::Mod: op = RegOp::Mod; break;
            case BinaryOp::Eq: op = RegOp::Eq; break;
            case BinaryOp::Ne: op = RegOp::Ne; break;
            case BinaryOp::Lt: op = RegOp::Lt; break;
            case BinaryOp::Le: op = RegOp::Le; break;
            case BinaryOp::Gt: op = RegOp::Gt; break;
            case BinaryOp::Ge: op = RegOp::Ge; break;
        }
        emit_rk(op, static_cast<std::uint32_t>(dest), left, right, binary.line);
        set_free_reg(save);
    }

    // `and` / `or` keep the deciding operand's value (notes §2.2): the left value is put in
    // `dest`, and the jump either keeps it or falls through to overwrite it with the right one.
    void compile(const Logical& logical, int dest) {
        if (dest_is_live_local(dest)) {
            int save = free_reg();
            int temp = reserve(logical.line);
            compile(logical, temp);
            move_operand_to(dest, {static_cast<std::uint32_t>(temp), false}, logical.line);
            set_free_reg(save);
            return;
        }
        expr(*logical.left, dest);
        std::size_t end = emit_jump(
            logical.op == LogicalOp::And ? RegOp::JumpIfFalse : RegOp::JumpIfTrue,
            static_cast<std::uint32_t>(dest), logical.line);
        expr(*logical.right, dest);
        patch_jump(end, logical.line);
    }

    // The callee goes in a register with its arguments in the registers right above it, and
    // the result comes back in the callee's register (Lua 5.0, section 4). If `dest` is the
    // newest temporary, the callee can sit in `dest` itself and no Move is needed after.
    void compile(const Call& call, int dest) {
        int save = free_reg();
        int base = dest_is_top_temp(dest) ? dest : reserve(call.line);
        expr(*call.callee, base);
        for (const ExprPtr& arg : call.args) {
            int reg = reserve(call.line);
            assert(reg == base + 1 + static_cast<int>(&arg - call.args.data()));
            expr(*arg, reg);
        }
        emit_abc(RegOp::Call, static_cast<std::uint32_t>(base),
                 static_cast<std::uint32_t>(call.args.size()), 0, call.line);
        if (base != dest) {
            move_operand_to(dest, {static_cast<std::uint32_t>(base), false}, call.line);
        }
        set_free_reg(save);
    }

    // Elements are evaluated into consecutive temporaries and gathered by one Array
    // instruction; literals longer than kArrayBatch are built a batch at a time (Lua's SETLIST
    // does the same), which keeps the registers needed bounded however long the literal is.
    void compile(const ArrayLiteral& array, int dest) {
        int save = free_reg();
        std::size_t count = array.elements.size();
        // A long literal is built in place over several instructions, so it must not be built
        // in a variable's register, where its own elements could read the half-built array.
        int target = count <= static_cast<std::size_t>(kArrayBatch) || dest_is_top_temp(dest)
                         ? dest
                         : reserve(array.line);
        int elements_start = free_reg();
        std::size_t next = 0;
        do {
            std::size_t batch = std::min(count - next, static_cast<std::size_t>(kArrayBatch));
            for (std::size_t i = 0; i < batch; ++i) {
                int reg = reserve(array.line);
                expr(*array.elements[next + i], reg);
            }
            emit_abc(next == 0 ? RegOp::Array : RegOp::ArrayAppend,
                     static_cast<std::uint32_t>(target), static_cast<std::uint32_t>(elements_start),
                     static_cast<std::uint32_t>(batch), array.line);
            set_free_reg(elements_start);
            next += batch;
        } while (next < count);
        if (target != dest) {
            move_operand_to(dest, {static_cast<std::uint32_t>(target), false}, array.line);
        }
        set_free_reg(save);
    }

    void compile(const Index& index, int dest) {
        int save = free_reg();
        Operand object = operand(*index.object, true, may_clobber_locals(*index.index));
        Operand position = operand(*index.index, true, false);
        emit_rk(RegOp::IndexGet, static_cast<std::uint32_t>(dest), object, position, index.line);
        set_free_reg(save);
    }

    // `dest` as for assign(): the assigned value's destination, or -1 to discard it.
    void index_assign(const IndexAssign& a, int dest) {
        int save = free_reg();
        // IndexSet's array operand is a plain register, so a literal is loaded into a temporary.
        Operand object = operand(*a.object, false,
                                 may_clobber_locals(*a.index) || may_clobber_locals(*a.value));
        Operand position = operand(*a.index, true, may_clobber_locals(*a.value));
        Operand value = operand(*a.value, true, false);
        emit_rk(RegOp::IndexSet, object.index, position, value, a.line);
        if (dest != -1) move_operand_to(dest, value, a.line);
        set_free_reg(save);
    }
    void compile(const IndexAssign& a, int dest) { index_assign(a, dest); }

    // ---- Statements ------------------------------------------------------------------------
    // At every statement boundary no temporaries are live: free_reg == the number of locals.
    void stmt(const Stmt& s) {
        assert(static_cast<std::size_t>(free_reg()) == current_->locals.size());
        std::visit([this](const auto& node) { compile(node); }, s.node);
    }

    void compile(const Print& print) {
        int save = free_reg();
        Operand value = operand(*print.value, true, false);
        emit_rk_b(RegOp::Print, 0, value, print.line);
        set_free_reg(save);
    }

    void compile(const ExprStmt& es) {
        int save = free_reg();
        const Expr& e = *es.expr;
        if (const auto* a = std::get_if<Assign>(&e.node)) {
            assign(*a, -1);
        } else if (const auto* ia = std::get_if<IndexAssign>(&e.node)) {
            index_assign(*ia, -1);
        } else {
            expr(e, reserve(es.line));  // value computed and thrown away
        }
        set_free_reg(save);
    }

    void compile(const Let& let) {
        // At the top level the value is moved into the globals table. In a scope it is computed
        // straight into the register the new local will own, which is declared only afterwards
        // so the initializer cannot see it.
        int save = free_reg();
        int reg = reserve(let.line);
        if (let.init) {
            expr(*let.init, reg);
        } else {
            emit_abc(RegOp::LoadNil, static_cast<std::uint32_t>(reg), 0, 0, let.line);
        }
        if (at_global_scope()) {
            emit(make_abx(RegOp::DefineGlobal, static_cast<std::uint32_t>(reg),
                          static_cast<std::uint32_t>(string_constant(let.name))),
                 let.line);
            set_free_reg(save);
        } else {
            add_local(let.name);
        }
    }

    void compile(const Block& block) {
        current_->scope_depth += 1;
        for (const StmtPtr& s : block.statements) stmt(*s);
        end_scope(block.line);
    }

    // The condition's register for JumpIfFalse (a local variable is used in place).
    std::size_t jump_if_false(const Expr& condition, int line) {
        int save = free_reg();
        Operand cond = operand(condition, false, false);
        std::size_t jump = emit_jump(RegOp::JumpIfFalse, cond.index, line);
        set_free_reg(save);
        return jump;
    }

    void compile(const If& branch) {
        std::size_t to_else = jump_if_false(*branch.condition, branch.line);
        stmt(*branch.then_branch);
        if (branch.else_branch) {
            std::size_t to_end = emit_jump(RegOp::Jump, 0, branch.line);
            patch_jump(to_else, branch.line);
            stmt(*branch.else_branch);
            patch_jump(to_end, branch.line);
        } else {
            patch_jump(to_else, branch.line);
        }
    }

    void compile(const While& loop) {
        std::size_t loop_start = here();
        std::size_t exit = jump_if_false(*loop.condition, loop.line);
        stmt(*loop.body);
        emit_loop(loop_start, loop.line);
        patch_jump(exit, loop.line);
    }

    void compile(const Return& ret) {
        if (ret.value) {
            int save = free_reg();
            Operand value = operand(*ret.value, true, false);
            emit_rk_b(RegOp::Return, 0, value, ret.line);
            set_free_reg(save);
        } else {
            emit_abc(RegOp::ReturnNil, 0, 0, 0, ret.line);
        }
    }

    void compile(const Function& fn) {
        int save = free_reg();
        int reg = reserve(fn.line);
        if (at_global_scope()) {
            compile_function(fn, reg);
            emit(make_abx(RegOp::DefineGlobal, static_cast<std::uint32_t>(reg),
                          static_cast<std::uint32_t>(string_constant(fn.name))),
                 fn.line);
            set_free_reg(save);
        } else {
            // Declared before the body so the function can call itself (notes D11); the
            // closure the Closure instruction creates lands in exactly this register.
            add_local(fn.name);
            compile_function(fn, reg);
        }
    }

    // Compiles a function body into its own RegChunk, then emits Closure (and its Capture
    // words) in the enclosing one, putting the closure in register `dest`.
    void compile_function(const Function& fn, int dest) {
        FunctionState state;
        begin_function(state, fn.name);
        // Parameters are registers 0..arity-1. They and the body's own statements share one
        // scope (notes §2.7).
        current_->scope_depth = 1;
        for (std::string_view param : fn.params) {
            reserve(fn.line);
            add_local(param);
        }
        current_->function->arity = static_cast<int>(fn.params.size());
        for (const StmtPtr& s : fn.body.statements) stmt(*s);
        emit_abc(RegOp::ReturnNil, 0, 0, 0, fn.line);  // falling off the end returns nil

        ObjFunction* done = state.function;
        done->upvalue_count = static_cast<int>(state.upvalues.size());
        current_ = state.enclosing;
        // `done` is rooted by nothing here, but nothing allocates until it is in the constants.
        std::size_t index = code().add_constant(make_obj(done));
        emit(make_abx(RegOp::Closure, static_cast<std::uint32_t>(dest),
                      static_cast<std::uint32_t>(index)),
             fn.line);
        for (const UpvalueRef& up : state.upvalues) {
            emit_abc(RegOp::Capture, up.is_local ? 1 : 0, up.index, 0, fn.line);
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
    emit_abc(RegOp::ReturnNil, 0, 0, 0, end_line);
    current_ = nullptr;
    return state.function;
}

RegCompileResult compile_register(const Program& program, Heap& heap) {
    Compiler compiler(heap);
    RootMarkerGuard guard(
        heap, heap.add_root_marker([&compiler](Heap& h) { compiler.mark_roots(h); }));
    RegCompileResult result;
    try {
        result.function = compiler.compile_script(program);
    } catch (const CompileError& error) {
        result.error = error;
    }
    return result;
}

}  // namespace rung
