#include "runtime/natives.h"

#include <chrono>
#include <cstdint>

namespace rung {

bool native_clock(Heap&, const Value*, Value* out, std::string*) {
    // steady_clock never goes backwards, unlike the wall clock. Only differences are meaningful.
    auto since_start = std::chrono::steady_clock::now().time_since_epoch();
    *out = make_float(std::chrono::duration<double>(since_start).count());
    return true;
}

bool native_len(Heap&, const Value* args, Value* out, std::string* error) {
    Value x = args[0];
    if (is_array(x)) {
        *out = make_int(static_cast<std::int32_t>(as_array(x)->elements.size()));
    } else if (is_string(x)) {
        // Length in bytes, not characters (notes §2.5).
        *out = make_int(static_cast<std::int32_t>(as_string(x)->chars.size()));
    } else {
        *error = "len expects an array or a string";
        return false;
    }
    return true;
}

bool native_array(Heap& heap, const Value* args, Value* out, std::string* error) {
    Value n = args[0];
    Value fill = args[1];
    if (!is_int(n) || as_int(n) < 0) {
        *error = "array size must be a non-negative int";
        return false;
    }
    // allocate() may collect before it builds the array, and `fill` would then be held only in
    // this local (the engine's copy may already have been popped).
    Heap::TempRoot keep_fill(heap, fill);
    *out = make_obj(heap.allocate<ObjArray>(static_cast<std::size_t>(as_int(n)), fill));
    return true;
}

void register_natives(Heap& heap,
                      const std::function<void(std::string_view name, Value native)>& define) {
    struct Spec {
        const char* name;
        int arity;
        NativeFn function;
    };
    static const Spec kSpecs[] = {
        {"clock", 0, native_clock},
        {"len", 1, native_len},
        {"array", 2, native_array},
    };
    for (const Spec& spec : kSpecs) {
        Value native = make_obj(heap.allocate<ObjNative>(spec.name, spec.arity, spec.function));
        Heap::TempRoot keep(heap, native);
        define(spec.name, native);
    }
}

}  // namespace rung
