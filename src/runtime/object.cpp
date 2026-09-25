#include "runtime/object.h"

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
    }
    return sizeof(Obj);
}

}  // namespace rung
