#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/value.h"

namespace rung {

class Heap;
struct Function;  // the AST node for a function declaration (ast.h)

// Later issues append kinds. Add the kind here, then to object_bytes (object.cpp), trace /
// free_object (heap.cpp) and print_object (ops.cpp). Function, Closure and Upvalue are the
// stack VM's; their structs live in runtime/function.h because they contain a Chunk.
// Environment and TreeFunction are the tree-walker's and are defined below.
enum class ObjKind : std::uint8_t {
    String, Array, Native, Function, Closure, Upvalue, Environment, TreeFunction
};

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

// One scope of the tree-walker: a block, or a function call's parameters plus body (notes D11).
// Names are interned strings, so the map hashes a pointer. `enclosing` is the scope this one
// was created inside; null means the next scope up is the globals table. The map grows after
// the object is linked, so object_bytes under-counts it until the next collection recounts.
struct ObjEnvironment : Obj {
    std::unordered_map<ObjString*, Value> vars;
    ObjEnvironment* enclosing;

    explicit ObjEnvironment(ObjEnvironment* outer) : Obj(ObjKind::Environment), enclosing(outer) {}
};

// A function as the tree-walker sees it: the AST node to run, plus the environment that was
// current where the `fn` statement executed (its closure). The AST is owned by the Program,
// which must outlive the heap objects that point into it.
struct ObjTreeFunction : Obj {
    ObjString* name;
    const Function* declaration;
    ObjEnvironment* closure;  // null: declared at top level, so free variables are globals

    ObjTreeFunction(ObjString* n, const Function* decl, ObjEnvironment* env)
        : Obj(ObjKind::TreeFunction), name(n), declaration(decl), closure(env) {}
};

// Bytes this object accounts for: sizeof the concrete type plus owned buffers (capacity, not
// size, since capacity is what is actually allocated).
std::size_t object_bytes(const Obj& obj);

inline bool is_obj_kind(Value v, ObjKind kind) { return is_obj(v) && as_obj(v)->kind == kind; }

inline bool is_string(Value v) { return is_obj_kind(v, ObjKind::String); }
inline bool is_array(Value v) { return is_obj_kind(v, ObjKind::Array); }
inline bool is_native(Value v) { return is_obj_kind(v, ObjKind::Native); }
inline bool is_tree_function(Value v) { return is_obj_kind(v, ObjKind::TreeFunction); }

inline ObjString* as_string(Value v) { return static_cast<ObjString*>(as_obj(v)); }
inline ObjArray* as_array(Value v) { return static_cast<ObjArray*>(as_obj(v)); }
inline ObjNative* as_native(Value v) { return static_cast<ObjNative*>(as_obj(v)); }
inline ObjTreeFunction* as_tree_function(Value v) {
    return static_cast<ObjTreeFunction*>(as_obj(v));
}

}  // namespace rung
