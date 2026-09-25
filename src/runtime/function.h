#pragma once

#include <vector>

#include "bytecode/chunk.h"
#include "runtime/object.h"

namespace rung {

// Stack-VM function objects. They are in their own header, not object.h, because ObjFunction
// contains a Chunk and chunk.h needs Value, which object.h is included from (value.h).
// The same three objects and the same upvalue design as clox, Crafting Interpreters ch. 25.

// A compiled function: the prototype every closure of it shares. Immutable once the compiler
// is done with it. The top-level program is one of these too, with no name and arity 0.
struct ObjFunction : Obj {
    ObjString* name;  // null for the top-level script
    int arity = 0;
    int upvalue_count = 0;
    Chunk chunk;

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
