#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "bytecode/chunk.h"
#include "engine.h"
#include "runtime/function.h"
#include "runtime/ops.h"
#include "vm/counters.h"

namespace rung {

// Instructions dispatched per stack opcode, and calls (notes D5).
using VmCounters = BasicVmCounters<kOpCodeCount>;

// Engine 2b: executes the stack bytecode that compile_stack produces (notes D12). A bytecode
// dispatch loop with a value stack and call frames, following clox in Crafting Interpreters
// (chapters 15, 24 and 25: the loop, calls and returns, closures and upvalues). Not followed
// from clox: the fixed-size stack and the 10,000-frame limit (below), the counters, the
// dispatch macros, and getting every arithmetic, comparison, printing and error-message rule
// from src/runtime/ops.h rather than writing it in the loop.
class StackEngine final : public Engine {
public:
    StackEngine(Heap& heap, Output& out);
    ~StackEngine() override;

    std::string_view name() const override { return "stack"; }
    EngineResult run(const Program& program) override;
    CallResult call_global(std::string_view name) override;
    std::string stats_report() const override;

    const VmCounters& counters() const { return counters_; }

private:
    // Value stack allocation (notes D4, issue #11). Open upvalues are raw pointers into the
    // stack, so the stack must never move. We allocate it once, at its maximum size, and never
    // grow it: room for 10,000 frames of 256 slots each (a frame holds slot 0 plus at most 255
    // locals, notes D11) plus kTempSlack extra slots for the temporaries an expression pushes.
    // That is 6,754,304 slots of 16 bytes (8 when NaN-boxed, notes D15) = about 103 MiB (52 MiB)
    // of address space, but the array is not initialised, so the operating system hands out
    // pages only as the stack is first touched: a program that never recurses deeply touches a few kilobytes. The alternative,
    // index-based upvalues on a std::vector that can grow, costs an add on every captured-
    // variable access and changes ObjUpvalue, which the later ladder rungs share.
    //
    // The price is a bound on temporaries. A call fails with `stack overflow` when fewer than
    // kFrameSlots + kTempSlack slots remain, so the temporaries of the running function (call
    // arguments, array-literal elements, pending operands) can use up to kTempSlack slots
    // without a per-push check. Reaching that limit takes an array literal of millions of
    // elements, or nesting far past what the front end allows, at the very deepest frame.
    static constexpr std::size_t kFrameSlots = 256;
    static constexpr std::size_t kTempSlack = std::size_t{1} << 22;
    static constexpr std::size_t kStackSlots =
        static_cast<std::size_t>(kMaxCallDepth) * kFrameSlots + kTempSlack;
    // One extra frame for the top-level script, which does not count towards the limit (§2.4).
    static constexpr std::size_t kMaxFrames = static_cast<std::size_t>(kMaxCallDepth) + 1;

    struct CallFrame {
        ObjClosure* closure;
        const std::uint8_t* ip;  // saved while another frame runs; the loop keeps it in a local
        Value* base;             // slot 0: the closure being run; locals follow
    };

    enum class CallOutcome { Error, PushedFrame, NativeDone };

    bool execute(std::size_t stop_frames);
    CallOutcome call_value(int argc);
    ObjUpvalue* capture_upvalue(Value* local);
    void close_upvalues(Value* last);
    void fail(std::string message);
    std::optional<RuntimeError> take_error();
    void mark_roots(Heap& heap);
    void push(Value v) { *sp_++ = v; }

    Heap& heap_;
    Output& out_;
    Heap::RootHandle root_handle_;

    std::unique_ptr<Value[]> stack_storage_;  // never value-initialised, see above
    Value* stack_;                            // first slot
    Value* sp_;                               // next free slot; saved from the loop's local
    Value* call_limit_;                       // a call needs this many free slots (see above)
    std::unique_ptr<CallFrame[]> frames_;
    std::size_t frame_count_ = 0;
    // Frames at the bottom that do not count as calls: 1 while the script runs, else 0.
    std::size_t uncounted_frames_ = 0;
    ObjUpvalue* open_upvalues_ = nullptr;  // sorted by stack address, highest first
    std::unordered_map<ObjString*, Value> globals_;
    Value result_ = make_nil();  // what the entry frame returned; not rooted
    std::optional<RuntimeError> error_;
    VmCounters counters_;
};

}  // namespace rung
