#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "bytecode/register_code.h"
#include "engine.h"
#include "runtime/function.h"
#include "runtime/ops.h"
#include "vm/counters.h"

namespace rung {

// Instructions dispatched per register opcode, and calls (notes D5).
using RegVmCounters = BasicVmCounters<kRegOpCount>;

// Engine 3 (ladder rung 3c): executes the register bytecode that compile_register produces
// (notes D14). The design follows "The Implementation of Lua 5.0" (Ierusalimschy, de Figueiredo,
// Celes), sections 5 and 7: one register file shared by every call, each frame a window into it,
// and a call that slides the window up instead of copying arguments. The dispatch macros, the
// Value interface and every semantic rule (src/runtime/ops.h) are the ones the stack VM uses
// (notes D4, D10).
//
// The register window. Register r of a running function is the slot `base + r`. A call
// `CALL A B` has put the callee in register A and its B arguments in A+1..A+B, so the callee's
// window starts at `base + A + 1`: its parameters are already in its registers 0..B-1, and
// nothing is copied. The callee's result is written to `base[-1]` of its own window, which is the
// caller's register A. The JIT builds on this (notes D3): a frame's registers are always in
// memory at `base + index`, and RegChunk::line_at maps an instruction index to its source line.
class RegisterEngine final : public Engine {
public:
    RegisterEngine(Heap& heap, Output& out);
    ~RegisterEngine() override;

    std::string_view name() const override { return "register"; }
    EngineResult run(const Program& program) override;
    CallResult call_global(std::string_view name) override;
    std::string stats_report() const override;

    const RegVmCounters& counters() const { return counters_; }

private:
    // Register file allocation: the same trade-off as the stack VM's value stack (see
    // vm_stack.h). Open upvalues are raw pointers to registers, so the file must never move: it
    // is allocated once, at its maximum size, and never grows, and the slots are not initialised,
    // so the operating system hands out pages only as frames first touch them. The size is the
    // stack VM's, 6,754,304 slots of 16 bytes (8 when NaN-boxed, notes D15).
    //
    // Unlike the stack VM, the register VM knows exactly how many slots a call needs (the
    // callee's frame_size, which counts every temporary, notes D14), so a call checks that the
    // callee's whole window fits and no slack for temporaries is needed. A call fails with
    // `stack overflow` when it does not fit; reaching that before the 10,000-frame limit takes
    // frames whose windows start, on average, more than 675 registers above their caller's,
    // i.e. calls made with hundreds of pending temporaries at every level of a deep recursion.
    // The alternative, register indices instead of pointers in open upvalues and a file that
    // can grow, costs an add on every captured-variable access and changes ObjUpvalue, which
    // both VMs share.
    static constexpr std::size_t kFrameSlots = 256;
    static constexpr std::size_t kRegisterSlots =
        static_cast<std::size_t>(kMaxCallDepth) * kFrameSlots + (std::size_t{1} << 22);
    // One extra frame for the top-level script, which does not count towards the limit (§2.4).
    static constexpr std::size_t kMaxFrames = static_cast<std::size_t>(kMaxCallDepth) + 1;

    struct CallFrame {
        ObjClosure* closure;
        const Instruction* pc;  // saved while another frame runs; the loop keeps it in a local
        Value* base;            // register 0; base[-1] is the caller's register for the result
    };

    enum class CallOutcome { Error, PushedFrame, NativeDone };

    bool execute(std::size_t stop_frames);
    CallOutcome call_value(Value* callee_slot, int argc);
    ObjUpvalue* capture_upvalue(Value* local);
    void close_upvalues(Value* last);
    void fail(std::string message);
    std::optional<RuntimeError> take_error();
    void mark_roots(Heap& heap);

    Heap& heap_;
    Output& out_;
    Heap::RootHandle root_handle_;

    std::unique_ptr<Value[]> register_storage_;  // never value-initialised, see above
    // Slot 0 holds the callee of the entry call (the script's closure, or the function
    // call_global calls), so the entry frame's window starts at slot 1 like every other window.
    // It is always a valid Value (nil when idle), so the collector can always mark it.
    Value* registers_;
    Value* register_end_;
    std::unique_ptr<CallFrame[]> frames_;
    std::size_t frame_count_ = 0;
    // Frames at the bottom that do not count as calls: 1 while the script runs, else 0.
    std::size_t uncounted_frames_ = 0;
    ObjUpvalue* open_upvalues_ = nullptr;  // sorted by register address, highest first
    std::unordered_map<ObjString*, Value> globals_;
    Value result_ = make_nil();  // what the entry frame returned; not rooted
    std::optional<RuntimeError> error_;
    RegVmCounters counters_;
};

}  // namespace rung
