#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/value.h"

namespace rung {

class Heap;

// Later issues append kinds (TreeFunction, Environment, Function, Closure, Upvalue). Add the
// kind here, then to object_bytes (object.cpp) and trace / free_object (heap.cpp).
enum class ObjKind : std::uint8_t { String, Array, Native };

// Common header. Every heap object derives from this and lives on the Heap's intrusive list.
// There is no virtual destructor: the Heap frees each object through its concrete type.
struct Obj {
    ObjKind kind;
    bool marked = false;
    Obj* next = nullptr;

    explicit Obj(ObjKind k) : kind(k) {}
    Obj(const Obj&) = delete;
    Obj& operator=(const Obj&) = delete;
};

// 32-bit FNV-1a over raw bytes.
std::uint32_t fnv1a(std::string_view bytes);

// Immutable bytes plus a cached hash. Only Heap::intern creates these, so equal contents
// means the same pointer (notes §2.2).
struct ObjString : Obj {
    std::string chars;
    std::uint32_t hash;

    explicit ObjString(std::string s)
        : Obj(ObjKind::String), chars(std::move(s)), hash(fnv1a(chars)) {}
};

// Fixed length after creation (notes D2). Elements are traced by the collector.
struct ObjArray : Obj {
    std::vector<Value> elements;

    explicit ObjArray(std::vector<Value> elems)
        : Obj(ObjKind::Array), elements(std::move(elems)) {}
    ObjArray(std::size_t count, Value fill) : Obj(ObjKind::Array), elements(count, fill) {}
};

// A host function. It may allocate through the Heap and report a runtime error by writing
// `*error` and returning false; on success it writes `*out` and returns true. The engine
// attaches the line number. Any Value the native holds only in a local across an allocation
// must be protected with a Heap::TempRoot.
using NativeFn = bool (*)(Heap& heap, const Value* args, Value* out, std::string* error);

struct ObjNative : Obj {
    std::string name;
    int arity;
    NativeFn function;

    ObjNative(std::string n, int a, NativeFn fn)
        : Obj(ObjKind::Native), name(std::move(n)), arity(a), function(fn) {}
};

// Bytes this object accounts for: sizeof the concrete type plus owned buffers (capacity, not
// size, since capacity is what is actually allocated).
std::size_t object_bytes(const Obj& obj);

inline bool is_obj_kind(Value v, ObjKind kind) { return is_obj(v) && as_obj(v)->kind == kind; }

inline bool is_string(Value v) { return is_obj_kind(v, ObjKind::String); }
inline bool is_array(Value v) { return is_obj_kind(v, ObjKind::Array); }
inline bool is_native(Value v) { return is_obj_kind(v, ObjKind::Native); }

inline ObjString* as_string(Value v) { return static_cast<ObjString*>(as_obj(v)); }
inline ObjArray* as_array(Value v) { return static_cast<ObjArray*>(as_obj(v)); }
inline ObjNative* as_native(Value v) { return static_cast<ObjNative*>(as_obj(v)); }

}  // namespace rung
