#include "vm_stack.h"

#include <cstdio>
#include <utility>
#include <vector>

#include "compiler_stack.h"
#include "disassembler.h"
#include "runtime/natives.h"
#include "runtime/ops.h"
#include "vm/dispatch.h"

namespace rung {

StackEngine::StackEngine(Heap& heap, Output& out)
    : heap_(heap),
      out_(out),
      // `new Value[n]` without braces leaves the slots uninitialised, so no page is touched.
      stack_storage_(new Value[kStackSlots]),
      stack_(stack_storage_.get()),
      sp_(stack_),
      call_limit_(stack_ + kStackSlots - kTempSlack),
      frames_(new CallFrame[kMaxFrames]) {
    // The root marker goes in first: register_natives allocates, and each native must already
    // be reachable through globals_ by the time the next allocation collects.
    root_handle_ = heap_.add_root_marker([this](Heap& h) { mark_roots(h); });
    register_natives(heap_, [this](std::string_view name, Value native) {
        globals_[heap_.intern(name)] = native;
    });
}

StackEngine::~StackEngine() { heap_.remove_root_marker(root_handle_); }

// What the collector must keep alive (notes D10): every live stack slot, the closure of every
// active frame (also in its slot 0, but cheap to be sure), the open upvalues (their values are
// stack slots, but the upvalue objects themselves are reachable from nothing else until a
// closure holds them), and the globals. Slots above sp_ hold stale values and are never read,
// so the loop must store its stack pointer into sp_ before anything that can allocate.
void StackEngine::mark_roots(Heap& heap) {
    for (const Value* slot = stack_; slot < sp_; ++slot) heap.mark_value(*slot);
    for (std::size_t i = 0; i < frame_count_; ++i) heap.mark_object(frames_[i].closure);
    for (ObjUpvalue* up = open_upvalues_; up != nullptr; up = up->next_open) heap.mark_object(up);
    for (auto& [name, value] : globals_) {
        heap.mark_object(name);
        heap.mark_value(value);
    }
}

// The error's line is the line of the instruction that failed: every byte of an instruction
// shares one line in the line table, so the byte just read (ip - 1) names it whether the
// failure came before or after its operands were decoded. The caller must have saved the
// running frame's ip first. With no frame (call_global failing before any code runs) there is
// no source line: 0, as the Engine interface says.
void StackEngine::fail(std::string message) {
    int line = 0;
    if (frame_count_ > 0) {
        const CallFrame& frame = frames_[frame_count_ - 1];
        const Chunk& chunk = frame.closure->function->chunk;
        line = chunk.line_at(static_cast<std::size_t>(frame.ip - chunk.code.data()) - 1);
    }
    error_ = RuntimeError{line, std::move(message)};
}

// Hands the error to the caller and puts the machine back to empty, closing every open upvalue
// first so a closure that outlives the failed run does not point into a stack that is reused.
std::optional<RuntimeError> StackEngine::take_error() {
    std::optional<RuntimeError> error = std::move(error_);
    error_.reset();
    if (error) {
        close_upvalues(stack_);
        sp_ = stack_;
        frame_count_ = 0;
    }
    return error;
}

// ---- upvalues (Crafting Interpreters ch. 25) -------------------------------------------------

// Returns the upvalue for the stack slot `local`, sharing the existing one if a closure already
// captured that slot: two closures over the same variable must see each other's writes.
ObjUpvalue* StackEngine::capture_upvalue(Value* local) {
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

// Closes every open upvalue at or above `last`: the variable is about to leave the stack, so
// its value moves into the upvalue object and the closures keep seeing it there.
void StackEngine::close_upvalues(Value* last) {
    while (open_upvalues_ != nullptr && open_upvalues_->location >= last) {
        ObjUpvalue* up = open_upvalues_;
        up->closed = *up->location;
        up->location = &up->closed;
        open_upvalues_ = up->next_open;
        up->next_open = nullptr;
    }
}

// ---- entry points ----------------------------------------------------------------------------

EngineResult StackEngine::run(const Program& program) {
    EngineResult result;
    StackCompileResult compiled = compile_stack(program, heap_);
    if (!compiled.ok()) {
        result.compile_error = compiled.error;
        return result;
    }
    // The script function is held only by `compiled` until it is on the stack, and nothing has
    // allocated since compile_stack returned. It stays rooted on the stack while the closure
    // is allocated, then the closure takes slot 0 of the script's frame.
    push(make_obj(compiled.function));
    ObjClosure* script = heap_.allocate<ObjClosure>(compiled.function);
    sp_ = stack_;
    push(make_obj(script));
    frames_[0] = CallFrame{script, compiled.function->chunk.code.data(), stack_};
    frame_count_ = 1;
    uncounted_frames_ = 1;  // the top-level script is not a call (notes §2.4)
    if (!execute(0)) result.runtime_error = take_error();
    return result;
}

CallResult StackEngine::call_global(std::string_view name) {
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
    push(it->second);  // rooted: it is on the stack
    CallOutcome outcome = call_value(0);
    if (outcome == CallOutcome::NativeDone) {
        result.value = sp_[-1];
        sp_ = stack_;
    } else if (outcome == CallOutcome::PushedFrame && execute(stop_frames)) {
        result.value = result_;
    } else {
        result.runtime_error = take_error();
    }
    return result;
}

std::string StackEngine::stats_report() const {
#if RUNG_VM_COUNTERS
    std::string text = "vm: " + std::to_string(counters_.instructions()) +
                       " instructions dispatched, " + std::to_string(counters_.rung_calls) +
                       " calls, " + std::to_string(counters_.native_calls) + " native calls\n";
    for (std::size_t op = 0; op < kOpCodeCount; ++op) {
        if (counters_.by_opcode[op] == 0) continue;
        char row[64];
        std::snprintf(row, sizeof row, "  %-14s %llu\n", opcode_name(static_cast<OpCode>(op)),
                      static_cast<unsigned long long>(counters_.by_opcode[op]));
        text += row;
    }
    return text;
#else
    return "vm: instruction counters are not compiled in (configure with -DRUNG_VM_COUNTERS=ON)\n";
#endif
}

// ---- calls -----------------------------------------------------------------------------------

// Calls the value below the `argc` arguments on top of the stack. For a Rung function it pushes
// a frame whose slot 0 is the callee and whose first parameter is the first argument already on
// the stack: no copying. A native runs at once and its result replaces the callee and arguments.
// The caller has saved the running frame's ip and sp_, so an error here reports the right line.
StackEngine::CallOutcome StackEngine::call_value(int argc) {
    Value callee = sp_[-argc - 1];
    if (is_closure(callee)) {
        ObjClosure* closure = as_closure(callee);
        int arity = closure->function->arity;
        if (arity != argc) {
            fail(arity_error_message(arity, argc));
            return CallOutcome::Error;
        }
        // Frame count, or stack space (see the note on kTempSlack in the header).
        if (frame_count_ - uncounted_frames_ >= static_cast<std::size_t>(kMaxCallDepth) ||
            sp_ + kFrameSlots > call_limit_) {
            fail(kErrStackOverflow);
            return CallOutcome::Error;
        }
        frames_[frame_count_++] =
            CallFrame{closure, closure->function->chunk.code.data(), sp_ - argc - 1};
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
        // The arguments stay on the stack (rooted) while the native runs and may allocate.
        if (!native->function(heap_, sp_ - argc, &result, &error)) {
            fail(std::move(error));
            return CallOutcome::Error;
        }
        sp_ -= argc + 1;
        push(result);
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
// instruction. The rule: before anything that can allocate (the GC reads sp_) or fail (fail()
// reads the frame's ip), SAVE_STATE() writes the locals back; after anything that changes the
// running frame or the stack pointer behind the loop's back, LOAD_FRAME() / `sp = sp_` reads
// them again.
#define READ_BYTE() (*ip++)
#define READ_U24() (ip += 3, get_u24(ip - 3))
#define SAVE_STATE() (frame->ip = ip, sp_ = sp)
#define LOAD_FRAME()                                             \
    do {                                                         \
        frame = &frames_[frame_count_ - 1];                      \
        closure = frame->closure;                                \
        ip = frame->ip;                                          \
        base = frame->base;                                      \
        constants = closure->function->chunk.constants.data();   \
    } while (false)
#define FAIL(message)       \
    do {                    \
        SAVE_STATE();       \
        fail(message);      \
        return false;       \
    } while (false)

#if RUNG_VM_COUNTERS
#define READ_OP() (++counters_.by_opcode[*ip], static_cast<OpCode>(*ip++))
#else
#define READ_OP() static_cast<OpCode>(READ_BYTE())
#endif

#define RUNG_FETCH() READ_OP()
#define RUNG_OP_ENUM OpCode
#define RUNG_OP_LIST RUNG_OPCODE_LIST
#define RUNG_COUNT_OP_(op) +1
static_assert(kOpCodeCount == 0 RUNG_OPCODE_LIST(RUNG_COUNT_OP_),
              "RUNG_OPCODE_LIST in vm/dispatch.h must list every OpCode, in enum order");
#undef RUNG_COUNT_OP_

// Runs until the frame count drops back to `stop_frames` (the entry frame returned) or a
// runtime error happens. Returns true with the entry frame's result in result_, or false with
// error_ set (the caller unwinds, see take_error). The entry frame must already be pushed.
bool StackEngine::execute(std::size_t stop_frames) {
    CallFrame* frame;
    ObjClosure* closure;
    const std::uint8_t* ip;
    Value* base;
    const Value* constants;
    Value* sp = sp_;
    LOAD_FRAME();

    RUNG_DISPATCH()

    RUNG_CASE(Const) {
        *sp++ = constants[READ_U24()];
        RUNG_NEXT();
    }
    RUNG_CASE(Nil) {
        *sp++ = make_nil();
        RUNG_NEXT();
    }
    RUNG_CASE(True) {
        *sp++ = make_bool(true);
        RUNG_NEXT();
    }
    RUNG_CASE(False) {
        *sp++ = make_bool(false);
        RUNG_NEXT();
    }
    RUNG_CASE(Pop) {
        --sp;
        RUNG_NEXT();
    }

    RUNG_CASE(GetLocal) {
        *sp++ = base[READ_BYTE()];
        RUNG_NEXT();
    }
    RUNG_CASE(SetLocal) {
        base[READ_BYTE()] = sp[-1];
        RUNG_NEXT();
    }
    RUNG_CASE(GetGlobal) {
        ObjString* name = as_string(constants[READ_U24()]);
        auto it = globals_.find(name);
        if (it == globals_.end()) FAIL(undefined_variable_message(name->chars));
        *sp++ = it->second;
        RUNG_NEXT();
    }
    RUNG_CASE(SetGlobal) {
        ObjString* name = as_string(constants[READ_U24()]);
        auto it = globals_.find(name);
        if (it == globals_.end()) FAIL(undefined_variable_message(name->chars));
        it->second = sp[-1];
        RUNG_NEXT();
    }
    RUNG_CASE(DefineGlobal) {
        ObjString* name = as_string(constants[READ_U24()]);
        globals_[name] = sp[-1];
        --sp;
        RUNG_NEXT();
    }
    RUNG_CASE(GetUpvalue) {
        *sp++ = *closure->upvalues[READ_BYTE()]->location;
        RUNG_NEXT();
    }
    RUNG_CASE(SetUpvalue) {
        *closure->upvalues[READ_BYTE()]->location = sp[-1];
        RUNG_NEXT();
    }

// Binary operators: the operands stay on the stack while the operation runs (op_add allocates
// for a string result), then both are replaced by the result.
#define BINARY_OP(call)                      \
    do {                                     \
        Value result;                        \
        std::string error;                   \
        SAVE_STATE();                        \
        if (!(call)) FAIL(std::move(error)); \
        --sp;                                \
        sp[-1] = result;                     \
    } while (false)

    RUNG_CASE(Add) {
        BINARY_OP(op_add(heap_, sp[-2], sp[-1], &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Sub) {
        BINARY_OP(op_sub(sp[-2], sp[-1], &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Mul) {
        BINARY_OP(op_mul(sp[-2], sp[-1], &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Div) {
        BINARY_OP(op_div(sp[-2], sp[-1], &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Mod) {
        BINARY_OP(op_mod(sp[-2], sp[-1], &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Lt) {
        BINARY_OP(op_less(sp[-2], sp[-1], &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Le) {
        BINARY_OP(op_less_equal(sp[-2], sp[-1], &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Gt) {
        BINARY_OP(op_greater(sp[-2], sp[-1], &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Ge) {
        BINARY_OP(op_greater_equal(sp[-2], sp[-1], &result, &error));
        RUNG_NEXT();
    }
    RUNG_CASE(Eq) {
        --sp;
        sp[-1] = make_bool(values_equal(sp[-1], sp[0]));
        RUNG_NEXT();
    }
    RUNG_CASE(Ne) {
        --sp;
        sp[-1] = make_bool(!values_equal(sp[-1], sp[0]));
        RUNG_NEXT();
    }
    // A handler's locals with destructors (std::string, std::vector) sit in an inner block that
    // closes before RUNG_NEXT(): a computed `goto *` may not jump out of such a variable's
    // scope, and the switch build would compile either way.
    RUNG_CASE(Neg) {
        {
            Value result;
            std::string error;
            if (!op_negate(sp[-1], &result, &error)) FAIL(std::move(error));
            sp[-1] = result;
        }
        RUNG_NEXT();
    }
    RUNG_CASE(Not) {
        sp[-1] = op_not(sp[-1]);
        RUNG_NEXT();
    }

    // Jump distances are measured from the byte after the operand (see OpCode::Jump).
    RUNG_CASE(Jump) {
        std::uint32_t distance = READ_U24();
        ip += distance;
        RUNG_NEXT();
    }
    RUNG_CASE(JumpIfFalse) {
        std::uint32_t distance = READ_U24();
        if (!is_truthy(sp[-1])) ip += distance;
        RUNG_NEXT();
    }
    RUNG_CASE(Loop) {
        std::uint32_t distance = READ_U24();
        ip -= distance;
        RUNG_NEXT();
    }

    RUNG_CASE(Call) {
        int argc = READ_BYTE();
        SAVE_STATE();
        CallOutcome outcome = call_value(argc);
        if (outcome == CallOutcome::Error) return false;
        sp = sp_;
        if (outcome == CallOutcome::PushedFrame) LOAD_FRAME();
        RUNG_NEXT();
    }
    RUNG_CASE(Closure) {
        ObjFunction* function = as_function(constants[READ_U24()]);
        SAVE_STATE();
        ObjClosure* created = heap_.allocate<ObjClosure>(function);
        // On the stack at once, so the allocations in capture_upvalue cannot collect it.
        *sp++ = make_obj(created);
        sp_ = sp;
        for (std::size_t i = 0; i < created->upvalues.size(); ++i) {
            std::uint8_t is_local = READ_BYTE();
            std::uint8_t index = READ_BYTE();
            created->upvalues[i] =
                is_local != 0 ? capture_upvalue(base + index) : closure->upvalues[index];
        }
        RUNG_NEXT();
    }
    RUNG_CASE(CloseUpvalue) {
        close_upvalues(sp - 1);
        --sp;
        RUNG_NEXT();
    }
    RUNG_CASE(Return) {
        Value result = sp[-1];
        close_upvalues(base);
        --frame_count_;
        if (frame_count_ == stop_frames) {
            result_ = result;
            sp_ = base;  // the callee and everything above it are gone
            return true;
        }
        sp = base;
        *sp++ = result;
        LOAD_FRAME();
        RUNG_NEXT();
    }

    RUNG_CASE(Print) {
        {
            std::string text;
            print_value(sp[-1], text);
            text += '\n';
            out_.write(text);
        }
        --sp;
        RUNG_NEXT();
    }
    RUNG_CASE(Array) {
        std::uint32_t count = READ_U24();
        SAVE_STATE();
        // The elements stay on the stack (rooted) while the array is allocated.
        ObjArray* array;
        {
            std::vector<Value> elements(sp - count, sp);
            array = heap_.allocate<ObjArray>(std::move(elements));
        }
        sp -= count;
        *sp++ = make_obj(array);
        RUNG_NEXT();
    }
    RUNG_CASE(IndexGet) {
        {
            Value result;
            std::string error;
            if (!array_get(sp[-2], sp[-1], &result, &error)) FAIL(std::move(error));
            --sp;
            sp[-1] = result;
        }
        RUNG_NEXT();
    }
    RUNG_CASE(IndexSet) {
        {
            std::string error;
            if (!array_set(sp[-3], sp[-2], sp[-1], &error)) FAIL(std::move(error));
        }
        sp -= 2;
        sp[-1] = sp[1];
        RUNG_NEXT();
    }

    RUNG_END_DISPATCH()
}

#undef READ_BYTE
#undef READ_U24
#undef SAVE_STATE
#undef LOAD_FRAME
#undef FAIL
#undef READ_OP
#undef RUNG_FETCH
#undef RUNG_OP_ENUM
#undef RUNG_OP_LIST
#undef BINARY_OP

}  // namespace rung
