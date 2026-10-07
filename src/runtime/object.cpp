#include "runtime/object.h"

#include "runtime/function.h"

namespace rung {

std::uint32_t fnv1a(std::string_view bytes) {
    std::uint32_t hash = 2166136261u;
    for (char c : bytes) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 16777619u;  // unsigned, so wraparound is well defined
    }
    return hash;
}

std::size_t object_bytes(const Obj& obj) {
    switch (obj.kind) {
        case ObjKind::String: {
            const auto& s = static_cast<const ObjString&>(obj);
            return sizeof(ObjString) + s.chars.capacity();
        }
        case ObjKind::Array: {
            const auto& a = static_cast<const ObjArray&>(obj);
            return sizeof(ObjArray) + a.elements.capacity() * sizeof(Value);
        }
        case ObjKind::Native: {
            const auto& n = static_cast<const ObjNative&>(obj);
            return sizeof(ObjNative) + n.name.capacity();
        }
        case ObjKind::Function: {
            // A function's chunk grows after the object is linked, so the heap's running total
            // under-counts it until the next collection recounts (sweep uses this function).
            const auto& f = static_cast<const ObjFunction&>(obj);
            return sizeof(ObjFunction) + f.chunk.owned_bytes();
        }
        case ObjKind::Closure: {
            const auto& c = static_cast<const ObjClosure&>(obj);
            return sizeof(ObjClosure) + c.upvalues.capacity() * sizeof(ObjUpvalue*);
        }
        case ObjKind::Upvalue:
            return sizeof(ObjUpvalue);
        case ObjKind::Environment: {
            // Rough: the map's nodes (key, value, next pointer) plus its bucket array.
            const auto& e = static_cast<const ObjEnvironment&>(obj);
            return sizeof(ObjEnvironment) +
                   e.vars.size() * (sizeof(ObjString*) + sizeof(Value) + sizeof(void*)) +
                   e.vars.bucket_count() * sizeof(void*);
        }
        case ObjKind::TreeFunction:
            return sizeof(ObjTreeFunction);
    }
    return sizeof(Obj);
}

}  // namespace rung
