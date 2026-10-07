#include "vm_reg.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>

#include "compiler_reg.h"
#include "disassembler.h"
#include "runtime/natives.h"
#include "runtime/ops.h"
#include "vm/dispatch.h"

namespace rung {

RegisterEngine::RegisterEngine(Heap& heap, Output& out, bool inline_cache, bool superinstructions)
    : heap_(heap),
      out_(out),
      // `new Value[n]` without braces leaves the slots uninitialised, so no page is touched.
      register_storage_(new Value[kRegisterSlots]),
      registers_(register_storage_.get()),
      register_end_(registers_ + kRegisterSlots),
      frames_(new CallFrame[kMaxFrames]),
      inline_cache_(inline_cache),
      superinstructions_(superinstructions) {
    registers_[0] = make_nil();
    // The root marker goes in first: register_natives allocates, and each native must already
    // be reachable through globals_ by the time the next allocation collects.
    root_handle_ = heap_.add_root_marker([this](Heap& h) { mark_roots(h); });
    register_natives(heap_, [this](std::string_view name, Value native) {
        globals_[heap_.intern(name)] = native;
    });
}

#if RUNG_JIT
RegisterEngine::RegisterEngine(Heap& heap, Output& out, const EngineOptions& options)
    : RegisterEngine(heap, out, options.inline_cache, options.superinstructions) {
    jit_ = std::make_unique<jit::Jit>(options.jit_threshold, options.jit_log);
}
#endif

RegisterEngine::~RegisterEngine() { heap_.remove_root_marker(root_handle_); }

// What the collector must keep alive (notes D10). The registers: every frame's window, which
// together are one contiguous run from slot 0 (each window starts inside its caller's, or right
// at its end), up to the highest window end. That is not always the newest frame's: a small
// callee can end below its caller. Every slot in that run holds a valid value, because a call
// sets the callee's registers above its arguments to nil before the frame runs (Lua 5.0 does the
// same). Slots above the run are stale (a returned callee's registers, possibly pointing at
// objects already freed) and are never marked; they are overwritten before any frame reads them.
// Then the closure of every frame, the open upvalues (reachable from nothing else until a closure
// holds them), and the globals.
void RegisterEngine::mark_roots(Heap& heap) {
    const Value* top = registers_ + 1;  // slot 0, the entry callee, is always valid
    for (std::size_t i = 0; i < frame_count_; ++i) {
        const CallFrame& frame = frames_[i];
        top = std::max<const Value*>(top, frame.base + frame.closure->function->reg.frame_size);
    }
    for (const Value* slot = registers_; slot < top; ++slot) heap.mark_value(*slot);
    for (std::size_t i = 0; i < frame_count_; ++i) heap.mark_object(frames_[i].closure);
    for (ObjUpvalue* up = open_upvalues_; up != nullptr; up = up->next_open) heap.mark_object(up);
    for (auto& [name, value] : globals_) {
        heap.mark_object(name);
        heap.mark_value(value);
    }
}

// The error's line is the line of the instruction that failed: the loop's pc has already moved
// past it, so it is the one at pc - 1. The caller must have saved the running frame's pc first.
// With no frame (call_global failing before any code runs) there is no source line: 0, as the
// Engine interface says.
void RegisterEngine::fail(std::string message) {
    int line = 0;
    if (frame_count_ > 0) {
        const CallFrame& frame = frames_[frame_count_ - 1];
        const RegChunk& chunk = frame.closure->function->reg;
        line = chunk.line_at(static_cast<std::size_t>(frame.pc - chunk.code.data()) - 1);
    }
    error_ = RuntimeError{line, std::move(message)};
}

// Hands the error to the caller and puts the machine back to empty, closing every open upvalue
// first so a closure that outlives the failed run does not point into registers that are reused.
std::optional<RuntimeError> RegisterEngine::take_error() {
    std::optional<RuntimeError> error = std::move(error_);
    error_.reset();
    if (error) {
        close_upvalues(registers_);
        frame_count_ = 0;
        registers_[0] = make_nil();
    }
    return error;
}

// ---- upvalues (Crafting Interpreters ch. 25; Lua 5.0 section 5) ------------------------------
// Exactly the stack VM's scheme, with registers in place of stack slots: while a captured
// variable's frame is live, its upvalue points at the register itself.

// Returns the upvalue for the register `local`, sharing the existing one if a closure already
// captured that register: two closures over the same variable must see each other's writes.
ObjUpvalue* RegisterEngine::capture_upvalue(Value* local) {
    ObjUpvalue* prev = nullptr;
    ObjUpvalue* up = open_upvalues_;
    while (up != nullptr && up->location > local) {
        prev = up;
        up = up->next_open;
    }
    if (up != nullptr && up->location == local) return up;
    // May collect; the open list is a root, so `prev` and `up` survive it.
    ObjUpvalue* created = heap_.allocate<ObjUpvalue>(local);
    created->next_open = up;
    if (prev == nullptr) {
        open_upvalues_ = created;
    } else {
        prev->next_open = created;
    }
    return created;
}

// Closes every open upvalue at or above `last`: the register is about to be reused, so its value
// moves into the upvalue object and the closures keep seeing it there.
void RegisterEngine::close_upvalues(Value* last) {
    while (open_upvalues_ != nullptr && open_upvalues_->location >= last) {
        ObjUpvalue* up = open_upvalues_;
        up->closed = *up->location;
        up->location = &up->closed;
        open_upvalues_ = up->next_open;
        up->next_open = nullptr;
    }
}

// ---- inline cache (ladder rung 3e) -----------------------------------------------------------

// Looks `name` up in the global table and returns its cell, or null if it is not defined. This is
// the slow path of GET_GLOBAL / SET_GLOBAL: with the inline cache it runs once per instruction,
// the first time that instruction executes. A lookup that fails caches nothing, so an undefined
// global keeps raising the same error each time the instruction runs, until it is defined.
Value* RegisterEngine::find_global(ObjString* name) {
    auto it = globals_.find(name);
    return it == globals_.end() ? nullptr : &it->second;
}

// Gives `function` and every function nested in it one cache slot per instruction, all empty.
// Nested functions are reachable only as constants of the function that contains them. Done once,
// before the code runs, so the dispatch loop never has to check that a cache exists.
void RegisterEngine::attach_global_caches(ObjFunction* function) {
    function->global_cache.assign(function->reg.code.size(), nullptr);
    for (const Value& constant : function->reg.constants) {
        if (is_function(constant)) attach_global_caches(as_function(constant));
    }
}

// ---- entry points ----------------------------------------------------------------------------

EngineResult RegisterEngine::run(const Program& program) {
    EngineResult result;
    RegCompileOptions compile_options;
    compile_options.superinstructions = superinstructions_;
    RegCompileResult compiled = compile_register(program, heap_, compile_options);
    if (!compiled.ok()) {
        result.compile_error = compiled.error;
        return result;
    }
    fused_pairs_ = compiled.fused_pairs;
    // The script function is held only by `compiled` until it is in slot 0 (always marked), and
    // nothing has allocated since compile_register returned. Then its closure replaces it there,
    // as the callee of the entry frame.
    if (inline_cache_) attach_global_caches(compiled.function);
    registers_[0] = make_obj(compiled.function);
    ObjClosure* script = heap_.allocate<ObjClosure>(compiled.function);
    registers_[0] = make_obj(script);
    // The script's window starts at slot 1, as a callee's would (see call_value). It is at most
    // 65,536 registers (notes D14), far less than the file holds.
    Value* base = registers_ + 1;
    const RegChunk& code = compiled.function->reg;
    assert(static_cast<std::size_t>(code.frame_size) < kRegisterSlots);
    std::fill(base, base + code.frame_size, make_nil());
    frames_[0] = CallFrame{script, code.code.data(), base};
    frame_count_ = 1;
    uncounted_frames_ = 1;  // the top-level script is not a call (notes §2.4)
    if (!execute(0)) result.runtime_error = take_error();
    registers_[0] = make_nil();
    return result;
}

CallResult RegisterEngine::call_global(std::string_view name) {
    CallResult result;
    // Interned for the lookup only; nothing allocates between here and the find, so the string
    // cannot be collected underneath us.
    ObjString* key = heap_.intern(name);
    auto it = globals_.find(key);
    if (it == globals_.end()) {
        result.runtime_error = RuntimeError{0, undefined_variable_message(name)};
        return result;
    }
    uncounted_frames_ = 0;  // unlike the script, this call counts towards the limit
    std::size_t stop_frames = frame_count_;
    registers_[0] = it->second;  // rooted: slot 0 is always marked
    CallOutcome outcome = call_value(registers_, 0);
    if (outcome == CallOutcome::NativeDone) {
        result.value = registers_[0];
    } else if (outcome == CallOutcome::PushedFrame && execute(stop_frames)) {
        result.value = result_;
    } else {
        result.runtime_error = take_error();
    }
    registers_[0] = make_nil();
    return result;
}

std::string RegisterEngine::stats_report() const {
    // A static count of the program, so it does not need the VM counters.
    std::string text;
    if (superinstructions_) {
        text += "superinstructions: pairs fused: " + std::to_string(fused_pairs_) + "\n";
    }
#if RUNG_JIT
    if (jit_ != nullptr) text += jit_->stats_report();
#endif
#if RUNG_VM_COUNTERS
    // With the JIT, instructions run as machine code are not dispatched and not counted.
    text += "vm: " + std::to_string(counters_.instructions()) + " instructions dispatched, " +
            std::to_string(counters_.rung_calls) + " calls, " +
            std::to_string(counters_.native_calls) + " native calls\n";
    if (inline_cache_) {
        text += "inline cache: " + std::to_string(counters_.global_cache_hits) + " hits, " +
                std::to_string(counters_.global_cache_misses) + " misses\n";
    }
    for (std::size_t op = 0; op < kRegOpCount; ++op) {
        if (counters_.by_opcode[op] == 0) continue;
        char row[64];
        std::snprintf(row, sizeof row, "  %-14s %llu\n", reg_opcode_name(static_cast<RegOp>(op)),
                      static_cast<unsigned long long>(counters_.by_opcode[op]));
        text += row;
    }
#else
    text += "vm: instruction counters are not compiled in (configure with -DRUNG_VM_COUNTERS=ON)\n";
#endif
    return text;
}

std::string RegisterEngine::pair_report() const {
#if RUNG_VM_COUNTERS
    return counters_.pair_report(
        [](std::size_t op) { return reg_opcode_name(static_cast<RegOp>(op)); });
#else
    return "pairs: instruction counters are not compiled in (configure with -DRUNG_VM_COUNTERS=ON)\n";
#endif
}

#if RUNG_JIT
// ---- the JIT's hooks (notes D16) -------------------------------------------------------------

// Hotness: one count per call and per loop back-edge (a backward JUMP), so a function that is
// called rarely but loops a lot, like a benchmark's `run`, still gets hot. At the threshold the
// function is compiled at once, on this thread. A back-edge cannot switch the running call into
// machine code (that would be on-stack replacement, not built), so a function that becomes hot
// inside a loop runs machine code from its next call. The top-level script is never compiled:
// it runs once, so without on-stack replacement its machine code could never be entered.
void RegisterEngine::jit_count(ObjFunction* function) {
    if (function->jit_status != JitStatus::Cold || function->name == nullptr) return;
    if (++function->jit_hotness >= jit_->threshold()) jit_->compile(*function);
}

// Called with the frame of `function` just pushed (pc at its first instruction). Counts the call
// and, if the function has machine code, runs it. The code works on the frame's registers in
// place and returns the index of the instruction the VM resumes at: a RETURN if it ran to the
// end (the VM performs the return), else the instruction whose guard failed, which the VM
// executes itself, error and all. Returns the pc to continue from.
const Instruction* RegisterEngine::jit_frame_entry(ObjFunction* function, Value* base,
                                                   const Instruction* pc) {
    jit_count(function);
    if (function->jit_entry == nullptr) return pc;
    const std::uint32_t resume = function->jit_entry(base);
    const Instruction* code = function->reg.code.data();
    RegOp op = insn_op(code[resume]);
    if (op != RegOp::Return && op != RegOp::ReturnNil) jit_->note_bailout(*function, resume);
    return code + resume;
}
#endif

// ---- calls -----------------------------------------------------------------------------------

// Calls the value in `callee_slot` with the `argc` arguments in the slots right above it. For a
// Rung function this slides the window: the new frame's base is callee_slot + 1, so the
// arguments are already its first registers. Its remaining registers are set to nil, so the
// collector never reads a stale or uninitialised slot (see mark_roots). A native runs at once
// and its result replaces the callee. The caller has saved the running frame's pc, so an error
// here reports the line of the CALL.
RegisterEngine::CallOutcome RegisterEngine::call_value(Value* callee_slot, int argc) {
    Value callee = *callee_slot;
    if (is_closure(callee)) {
        ObjClosure* closure = as_closure(callee);
        const ObjFunction* function = closure->function;
        int arity = function->arity;
        if (arity != argc) {
            fail(arity_error_message(arity, argc));
            return CallOutcome::Error;
        }
        Value* base = callee_slot + 1;
        auto frame_size = static_cast<std::size_t>(function->reg.frame_size);
        // Frame count, or room for the callee's whole window (see kRegisterSlots).
        if (frame_count_ - uncounted_frames_ >= static_cast<std::size_t>(kMaxCallDepth) ||
            frame_size > static_cast<std::size_t>(register_end_ - base)) {
            fail(kErrStackOverflow);
            return CallOutcome::Error;
        }
        std::fill(base + arity, base + frame_size, make_nil());
        frames_[frame_count_++] = CallFrame{closure, function->reg.code.data(), base};
#if RUNG_VM_COUNTERS
        counters_.rung_calls += 1;
#endif
        return CallOutcome::PushedFrame;
    }
    if (is_native(callee)) {
        ObjNative* native = as_native(callee);
        if (native->arity != argc) {
            fail(arity_error_message(native->arity, argc));
            return CallOutcome::Error;
        }
        Value result;
        std::string error;
        // The arguments stay in the caller's registers (rooted) while the native runs and may
        // allocate.
        if (!native->function(heap_, callee_slot + 1, &result, &error)) {
            fail(std::move(error));
            return CallOutcome::Error;
        }
        *callee_slot = result;
#if RUNG_VM_COUNTERS
        counters_.native_calls += 1;
#endif
        return CallOutcome::NativeDone;
    }
    fail(kErrNotCallable);
    return CallOutcome::Error;
}

// ---- the dispatch loop -----------------------------------------------------------------------

// The frame state lives in locals so the loop does not reload it from the engine on every
// instruction. The rule: before anything that can fail (fail() reads the frame's pc) or call,
// SAVE_STATE() writes the pc back; after anything that changes the running frame,
// LOAD_FRAME() reads the locals again. Unlike the stack VM there is no stack pointer to save
// before an allocation: the collector finds the registers through the frames themselves.
//
// Every handler reads all of its operands before it writes R[A], because the compiler may name
// the same register as source and destination (`ADD r0 r0 k1`, notes D14).
#define SAVE_STATE() (frame->pc = pc)
#define LOAD_FRAME()                                             \
    do {                                                         \
        frame = &frames_[frame_count_ - 1];                      \
        closure = frame->closure;                                \
        pc = frame->pc;                                          \
        base = frame->base;                                      \
        constants = closure->function->reg.constants.data();     \
        if constexpr (kInlineCache) {                            \
            code_begin = closure->function->reg.code.data();     \
            global_cache = closure->function->global_cache.data();\
        }                                                        \
    } while (false)
#define FAIL(message)       \
    do {                    \
        SAVE_STATE();       \
        fail(message);      \
        return false;       \
    } while (false)

// Makes `slot` (a cache slot, a Value*&) point at the cell of the global `name`, looking it up
// only when the slot is empty; an undefined global is the runtime error and the slot stays empty.
#if RUNG_VM_COUNTERS
#define COUNT_CACHE(field) (counters_.field += 1)
#else
#define COUNT_CACHE(field) ((void)0)
#endif
#define GLOBAL_CACHE_LOOKUP(slot, name)                      \
    do {                                                     \
        if ((slot) != nullptr) {                             \
            COUNT_CACHE(global_cache_hits);                  \
        } else {                                             \
            (slot) = find_global(name);                      \
            if ((slot) == nullptr) {                         \
                FAIL(undefined_variable_message((name)->chars)); \
            }                                                \
            COUNT_CACHE(global_cache_misses);                \
        }                                                    \
    } while (false)

#define REG_A() base[insn_a(insn)]
#define REG_B() base[insn_b(insn)]
// RK operands (notes D14): a register, or a constant when the instruction's flag bit says so.
#define RK_B()                                                                       \
    ((insn_flags(insn) & kFlagBConst) != 0 ? constants[insn_b(insn)] : base[insn_b(insn)])
#define RK_C()                                                                       \
    ((insn_flags(insn) & kFlagCConst) != 0 ? constants[insn_c(insn)] : base[insn_c(insn)])

// The body of each instruction that can be the half of a superinstruction (ladder rung 3d). The
// plain handler and the fused handlers expand the same macro, so there is one copy of each rule.
// A body reads the operands of the instruction in `insn` and may fail.
// Operations that can fail: the operands are read (op_add allocates for a string result, and
// operands in registers or constants stay rooted through the frame), then the result goes to
// R[A]. The do-block also closes the std::string's scope before RUNG_NEXT() (see below).
#define CHECKED_OP(call)                     \
    do {                                     \
        Value result;                        \
        std::string error;                   \
        if (!(call)) FAIL(std::move(error)); \
        REG_A() = result;                    \
    } while (false)
#define BODY_ADD() CHECKED_OP(op_add(heap_, RK_B(), RK_C(), &result, &error))
#define BODY_DIV() CHECKED_OP(op_div(RK_B(), RK_C(), &result, &error))
#define BODY_MOD() CHECKED_OP(op_mod(RK_B(), RK_C(), &result, &error))
#define BODY_LT() CHECKED_OP(op_less(RK_B(), RK_C(), &result, &error))
#define BODY_LE() CHECKED_OP(op_less_equal(RK_B(), RK_C(), &result, &error))
// Offsets count instructions from the one after the jump, which is where pc already is.
#define BODY_JUMP()                \
    do {                           \
        JIT_BACK_EDGE();           \
        pc += insn_sbx(insn);      \
    } while (false)
#define BODY_JUMP_IF_FALSE() (!is_truthy(REG_A()) ? (void)(pc += insn_sbx(insn)) : (void)0)
#define BODY_INDEX_SET()                                                          \
    do {                                                                          \
        std::string error;                                                        \
        if (!array_set(REG_A(), RK_B(), RK_C(), &error)) FAIL(std::move(error));  \
    } while (false)

#if RUNG_VM_COUNTERS
#define RUNG_FETCH() (insn = *pc++, counters_.count_dispatch(insn & 0xFFu), insn_op(insn))
#else
#define RUNG_FETCH() (insn = *pc++, insn_op(insn))
#endif
#define RUNG_OP_ENUM RegOp
#define RUNG_OP_LIST RUNG_REG_OPCODE_LIST
#define RUNG_COUNT_OP_(op) +1
static_assert(kRegOpCount == 0 RUNG_REG_OPCODE_LIST(RUNG_COUNT_OP_),
              "RUNG_REG_OPCODE_LIST in vm/dispatch.h must list every RegOp, in enum order");
#undef RUNG_COUNT_OP_

// Runs until the frame count drops back to `stop_frames` (the entry frame returned) or a
// runtime error happens. Returns true with the entry frame's result in result_, or false with
// error_ set (the caller unwinds, see take_error). The entry frame must already be pushed.
bool RegisterEngine::execute(std::size_t stop_frames) {
    counters_.restart_pairs();
#if RUNG_JIT
    if (jit_ != nullptr) {
        return inline_cache_ ? execute_loop<true, true>(stop_frames)
                             : execute_loop<false, true>(stop_frames);
    }
#endif
    return inline_cache_ ? execute_loop<true, false>(stop_frames)
                         : execute_loop<false, false>(stop_frames);
}

// The JIT's two hooks in the loop (notes D16). They compile to nothing when kJit is false.
// JIT_FRAME_ENTRY runs after a frame is pushed: it counts the call and, if the function has
// machine code, runs it and moves pc to where the code stopped. JIT_BACK_EDGE counts a
// backward jump.
#if RUNG_JIT
#define JIT_FRAME_ENTRY()                                                       \
    do {                                                                        \
        if constexpr (kJit) pc = jit_frame_entry(closure->function, base, pc);  \
    } while (false)
#define JIT_BACK_EDGE()                                                         \
    do {                                                                        \
        if constexpr (kJit) {                                                   \
            if (insn_sbx(insn) < 0) jit_count(closure->function);               \
        }                                                                       \
    } while (false)
#else
#define JIT_FRAME_ENTRY() ((void)0)
#define JIT_BACK_EDGE() ((void)0)
#endif

template <bool kInlineCache, bool kJit>
bool RegisterEngine::execute_loop(std::size_t stop_frames) {
    CallFrame* frame;
    ObjClosure* closure;
    const Instruction* pc;
    Value* base;
    const Value* constants;
    Instruction insn;
    // The running function's instructions and its cache, which has one slot per instruction
    // (ObjFunction::global_cache). Only the cached instantiation uses them.
    [[maybe_unused]] const Instruction* code_begin = nullptr;
    [[maybe_unused]] Value** global_cache = nullptr;
    LOAD_FRAME();
    // call_global's entry frame is a call like any other (run()'s script frame is never
    // compiled; see jit_count).
    JIT_FRAME_ENTRY();

    RUNG_DISPATCH()

    RUNG_CASE(Move) {
        REG_A() = REG_B();
        RUNG_NEXT();
    }
    RUNG_CASE(LoadK) {
        REG_A() = constants[insn_bx(insn)];
        RUNG_NEXT();
    }
    RUNG_CASE(LoadNil) {
        REG_A() = make_nil();
        RUNG_NEXT();
    }
    RUNG_CASE(LoadTrue) {
        REG_A() = make_bool(true);
        RUNG_NEXT();
    }
    RUNG_CASE(LoadFalse) {
        REG_A() = make_bool(false);
        RUNG_NEXT();
    }

    // With the inline cache, each of these instructions owns the slot at its own index in the
    // function's cache (`pc` has already moved past it, hence the - 1). An empty slot is a
    // miss: look the name up, and remember the cell only if it exists, so an undefined global
    // is found undefined again the next time. A full slot is a hit and skips the hash lookup.
    // The cell never moves and the global never stops being defined, so a slot never goes
    // stale (see globals_ in vm_reg.h). Without the cache these are the plain lookups.
    RUNG_CASE(GetGlobal) {
        ObjString* name = as_string(constants[insn_bx(insn)]);
        if constexpr (kInlineCache) {
            Value*& slot = global_cache[(pc - 1) - code_begin];
            GLOBAL_CACHE_LOOKUP(slot, name);
            REG_A() = *slot;
        } else {
            auto it = globals_.find(name);
            if (it == globals_.end()) FAIL(undefined_variable_message(name->chars));
            REG_A() = it->second;
        }
        RUNG_NEXT();
    }
    RUNG_CASE(SetGlobal) {
        ObjString* name = as_string(constants[insn_bx(insn)]);
        if constexpr (kInlineCache) {
            Value*& slot = global_cache[(pc - 1) - code_begin];
            GLOBAL_CACHE_LOOKUP(slot, name);
            *slot = REG_A();
        } else {
            auto it = globals_.find(name);
            if (it == globals_.end()) FAIL(undefined_variable_message(name->chars));
            it->second = REG_A();
        }
        RUNG_NEXT();
    }
    RUNG_CASE(DefineGlobal) {
        globals_[as_string(constants[insn_bx(insn)])] = REG_A();
        RUNG_NEXT();
    }
    RUNG_CASE(GetUpvalue) {
        REG_A() = *closure->upvalues[insn_b(insn)]->location;
        RUNG_NEXT();
    }
    RUNG_CASE(SetUpvalue) {
        *closure->upvalues[insn_b(insn)]->location = REG_A();
        RUNG_NEXT();
    }

    // A handler's locals with destructors (std::string, std::vector) sit in an inner block that
    // closes before RUNG_NEXT(): a computed `goto *` may not jump out of such a variable's
    // scope, and the switch build would compile either way.
    RUNG_CASE(Add) {
        BODY_ADD();
        RUNG_NEXT();
    }
    RUNG_CASE(Sub) {
        CHECKED_OP(op_sub(RK_B(), RK_C(), &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Mul) {
        CHECKED_OP(op_mul(RK_B(), RK_C(), &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Div) {
        BODY_DIV();
        RUNG_NEXT();
    }
    RUNG_CASE(Mod) {
        BODY_MOD();
        RUNG_NEXT();
    }
    RUNG_CASE(Eq) {
        REG_A() = make_bool(values_equal(RK_B(), RK_C()));
        RUNG_NEXT();
    }
    RUNG_CASE(Ne) {
        REG_A() = make_bool(!values_equal(RK_B(), RK_C()));
        RUNG_NEXT();
    }
    RUNG_CASE(Lt) {
        BODY_LT();
        RUNG_NEXT();
    }
    RUNG_CASE(Le) {
        BODY_LE();
        RUNG_NEXT();
    }
    RUNG_CASE(Gt) {
        CHECKED_OP(op_greater(RK_B(), RK_C(), &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Ge) {
        CHECKED_OP(op_greater_equal(RK_B(), RK_C(), &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Neg) {
        CHECKED_OP(op_negate(RK_B(), &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Not) {
        REG_A() = op_not(RK_B());
        RUNG_NEXT();
    }

    // Offsets count instructions from the one after the jump, which is where pc already is.
    RUNG_CASE(Jump) {
        BODY_JUMP();
        RUNG_NEXT();
    }
    RUNG_CASE(JumpIfFalse) {
        BODY_JUMP_IF_FALSE();
        RUNG_NEXT();
    }
    RUNG_CASE(JumpIfTrue) {
        if (is_truthy(REG_A())) pc += insn_sbx(insn);
        RUNG_NEXT();
    }

    RUNG_CASE(Call) {
        SAVE_STATE();
        CallOutcome outcome = call_value(&REG_A(), static_cast<int>(insn_b(insn)));
        if (outcome == CallOutcome::Error) return false;
        if (outcome == CallOutcome::PushedFrame) {
            LOAD_FRAME();
            JIT_FRAME_ENTRY();
        }
        RUNG_NEXT();
    }
    RUNG_CASE(Closure) {
        ObjFunction* function = as_function(constants[insn_bx(insn)]);
        // The function is reachable from this frame's closure while the new closure is
        // allocated. The closure goes into R[A] (inside this frame's window, so rooted) before
        // capture_upvalue allocates. A local function's own register can be among its captures
        // (it calls itself); the upvalue points at the register, which now holds the closure.
        ObjClosure* created = heap_.allocate<ObjClosure>(function);
        REG_A() = make_obj(created);
        for (std::size_t i = 0; i < created->upvalues.size(); ++i) {
            Instruction capture = *pc++;  // operands of CLOSURE, never dispatched
            created->upvalues[i] = insn_a(capture) != 0 ? capture_upvalue(base + insn_b(capture))
                                                        : closure->upvalues[insn_b(capture)];
        }
        RUNG_NEXT();
    }
    RUNG_CASE(Capture) {
        // Unreachable: Closure consumes its Capture words. The case exists because every opcode
        // needs a label in the dispatch table.
        RUNG_NEXT();
    }
    RUNG_CASE(Close) {
        close_upvalues(&REG_A());
        RUNG_NEXT();
    }
    RUNG_CASE(Return) {
        Value result = RK_B();
        close_upvalues(base);
        --frame_count_;
        if (frame_count_ == stop_frames) {
            result_ = result;
            return true;
        }
        base[-1] = result;  // the caller's register A, which held the callee
        LOAD_FRAME();
        RUNG_NEXT();
    }
    RUNG_CASE(ReturnNil) {
        close_upvalues(base);
        --frame_count_;
        if (frame_count_ == stop_frames) {
            result_ = make_nil();
            return true;
        }
        base[-1] = make_nil();
        LOAD_FRAME();
        RUNG_NEXT();
    }

    RUNG_CASE(Print) {
        {
            std::string text;
            print_value(RK_B(), text);
            text += '\n';
            out_.write(text);
        }
        RUNG_NEXT();
    }
    RUNG_CASE(Array) {
        {
            // The elements stay in their registers (rooted) while the array is allocated.
            Value* first = &REG_B();
            std::vector<Value> elements(first, first + insn_c(insn));
            ObjArray* array = heap_.allocate<ObjArray>(std::move(elements));
            REG_A() = make_obj(array);
        }
        RUNG_NEXT();
    }
    RUNG_CASE(ArrayAppend) {
        {
            // Grows a literal longer than one batch (notes D14). The vector grows after the
            // object was linked, so the heap's byte count catches up at the next collection, as
            // for the tree-walker's environments.
            Value* first = &REG_B();
            std::vector<Value>& elements = as_array(REG_A())->elements;
            elements.insert(elements.end(), first, first + insn_c(insn));
        }
        RUNG_NEXT();
    }
    RUNG_CASE(IndexGet) {
        {
            Value result;
            std::string error;
            if (!array_get(RK_B(), RK_C(), &result, &error)) FAIL(std::move(error));
            REG_A() = result;
        }
        RUNG_NEXT();
    }
    RUNG_CASE(IndexSet) {
        BODY_INDEX_SET();
        RUNG_NEXT();
    }

    // Superinstructions (ladder rung 3d, bytecode/register_code.h "Fusion"). Each runs the body
    // of its first half, which is the instruction in this word, then takes the word after it
    // (`insn = *pc++`) and runs the body of its second half exactly as that instruction's own
    // handler would: same macros, same operand decoding, and a failure in either half reports the
    // line of the half that failed, because `pc` has moved past exactly the words consumed. The
    // bodies are the ones the plain handlers above use, so a rule exists once. What is saved is
    // the dispatch between the halves. (If a global access is ever fused, its first half must
    // run before `pc` moves, so it still uses the cache slot at the fused word's own index.)
#define SECOND_HALF() (insn = *pc++)
    RUNG_CASE(LtJumpIfFalse) {
        BODY_LT();
        SECOND_HALF();
        BODY_JUMP_IF_FALSE();
        RUNG_NEXT();
    }
    RUNG_CASE(LeJumpIfFalse) {
        BODY_LE();
        SECOND_HALF();
        BODY_JUMP_IF_FALSE();
        RUNG_NEXT();
    }
    RUNG_CASE(AddJump) {
        BODY_ADD();
        SECOND_HALF();
        BODY_JUMP();
        RUNG_NEXT();
    }
    RUNG_CASE(ModAdd) {
        BODY_MOD();
        SECOND_HALF();
        BODY_ADD();
        RUNG_NEXT();
    }
    RUNG_CASE(DivAdd) {
        BODY_DIV();
        SECOND_HALF();
        BODY_ADD();
        RUNG_NEXT();
    }
    RUNG_CASE(IndexSetAdd) {
        BODY_INDEX_SET();
        SECOND_HALF();
        BODY_ADD();
        RUNG_NEXT();
    }
#undef SECOND_HALF

    RUNG_END_DISPATCH()
}

#undef SAVE_STATE
#undef LOAD_FRAME
#undef FAIL
#undef GLOBAL_CACHE_LOOKUP
#undef COUNT_CACHE
#undef REG_A
#undef REG_B
#undef RK_B
#undef RK_C
#undef RUNG_FETCH
#undef RUNG_OP_ENUM
#undef RUNG_OP_LIST
#undef CHECKED_OP
#undef BODY_ADD
#undef BODY_DIV
#undef BODY_MOD
#undef BODY_LT
#undef BODY_LE
#undef BODY_JUMP
#undef BODY_JUMP_IF_FALSE
#undef BODY_INDEX_SET
#undef JIT_FRAME_ENTRY
#undef JIT_BACK_EDGE

}  // namespace rung
