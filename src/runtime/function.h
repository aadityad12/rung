#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "bytecode/chunk.h"
#include "bytecode/register_code.h"
#include "runtime/object.h"

namespace rung {

// Machine code the baseline JIT produced for one function (notes D16). It takes the frame's
// register 0 (`base`) and returns the index of the bytecode instruction the register VM resumes
// at: a RETURN / RETURN_NIL when the code ran to the end, or the instruction whose type guard or
// zero-divisor check failed.
using JitEntry = std::uint32_t (*)(Value* base);

// Where a function stands with the JIT, as the engine's own thread sees it. Only --engine=jit
// changes it, and only that thread reads or writes it (the background compiler never does).
enum class JitStatus : std::uint8_t {
    Cold,      // still counting calls and loop back-edges
    Queued,    // --jit-background: handed to the compiler thread, which publishes jit_entry if
               // the function compiles and never touches this field (notes D8)
    Compiled,  // jit_entry is set and this thread has seen it; every later call runs machine code
    Rejected,  // outside the whitelist (or the code could not be emitted); stays in the VM
};

// Stack-VM function objects. They are in their own header, not object.h, because ObjFunction
// contains a Chunk and chunk.h needs Value, which object.h is included from (value.h).
// The same three objects and the same upvalue design as clox, Crafting Interpreters ch. 25.

// A compiled function: the prototype every closure of it shares. Immutable once the compiler
// is done with it. The top-level program is one of these too, with no name and arity 0.
// It carries the code of whichever compiler produced it: `chunk` from the stack compiler, or
// `reg` from the register compiler (notes D14). The other one stays empty.
struct ObjFunction : Obj {
    ObjString* name;  // null for the top-level script
    int arity = 0;
    int upvalue_count = 0;
    Chunk chunk;
    RegChunk reg;
    // Run-time state of the register VM's inline cache (ladder rung 3e, notes §5), not part of
    // the compiled code: one slot per instruction of `reg`, null until that GET_GLOBAL or
    // SET_GLOBAL first finds its global. A slot points at the global's cell in the engine's
    // table, which never moves. Empty unless the engine runs with --inline-cache. Only the VM's
    // own thread reads or writes it, so the bytecode itself stays immutable and safe to read
    // from the background compiler thread (notes D8).
    std::vector<Value*> global_cache;
    // Baseline JIT state (notes D16). Kept on the function so the check on every call is a load
    // and a compare, not a table lookup. The machine code itself is owned by the JIT, which
    // outlives every function it compiled. Hotness and status belong to the engine's thread.
    std::uint32_t jit_hotness = 0;  // calls plus loop back-edges counted so far
    JitStatus jit_status = JitStatus::Cold;
    // The handoff (notes D8): the only field of a function the background compiler writes. It
    // stores the finished code here with a release store, after the code is written and the
    // instruction cache flushed; the engine's thread reads it with an acquire load before every
    // call, so seeing the pointer guarantees seeing the code it points to.
    std::atomic<JitEntry> jit_entry{nullptr};

    explicit ObjFunction(ObjString* n) : Obj(ObjKind::Function), name(n) {}
};

// A variable a closure captured. While the variable still lives on the VM's stack it is
// "open": `location` points at that stack slot and every closure sharing it sees writes.
// When the slot is about to go away the VM copies the value into `closed` and points
// `location` at it. `next_open` links the VM's list of open upvalues (sorted by stack slot);
// it is meaningless once closed.
struct ObjUpvalue : Obj {
    Value* location;
    Value closed = make_nil();
    ObjUpvalue* next_open = nullptr;

    explicit ObjUpvalue(Value* slot) : Obj(ObjKind::Upvalue), location(slot) {}
};

// A function plus the upvalues it captured. The VM fills `upvalues` right after creation, as
// the CLOSURE instruction reads its (is_local, index) pairs; until then the entries are null.
struct ObjClosure : Obj {
    ObjFunction* function;
    std::vector<ObjUpvalue*> upvalues;

    explicit ObjClosure(ObjFunction* fn)
        : Obj(ObjKind::Closure), function(fn),
          upvalues(static_cast<std::size_t>(fn->upvalue_count), nullptr) {}
};

inline bool is_function(Value v) { return is_obj_kind(v, ObjKind::Function); }
inline bool is_closure(Value v) { return is_obj_kind(v, ObjKind::Closure); }
inline bool is_upvalue(Value v) { return is_obj_kind(v, ObjKind::Upvalue); }

inline ObjFunction* as_function(Value v) { return static_cast<ObjFunction*>(as_obj(v)); }
inline ObjClosure* as_closure(Value v) { return static_cast<ObjClosure*>(as_obj(v)); }
inline ObjUpvalue* as_upvalue(Value v) { return static_cast<ObjUpvalue*>(as_obj(v)); }

}  // namespace rung
