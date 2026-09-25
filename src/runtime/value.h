#pragma once

#include <cstdint>

namespace rung {

struct Obj;

// The tagged-struct Value: a 1-byte type tag plus an 8-byte union. Alignment pads the tag out
// to 8 bytes, so the whole thing is 16 bytes (notes D4, D10). A later NaN-boxed build swaps
// this struct for an 8-byte one behind exactly the free functions below, so nothing outside
// this header may touch the members.
enum class ValueType : std::uint8_t { Nil, Bool, Int, Float, Obj };

struct Value {
    // Private by convention: use the free functions only.
    ValueType type_;
    union {
        bool bool_;
        std::int32_t int_;
        double float_;
        Obj* obj_;
    };
};

static_assert(sizeof(Value) == 16, "the tagged-struct Value is 16 bytes (notes D4)");

inline Value make_nil() {
    Value v;
    v.type_ = ValueType::Nil;
    v.float_ = 0.0;  // initialise the whole union so copies never read indeterminate bytes
    return v;
}
inline Value make_bool(bool b) {
    Value v = make_nil();
    v.type_ = ValueType::Bool;
    v.bool_ = b;
    return v;
}
inline Value make_int(std::int32_t i) {
    Value v = make_nil();
    v.type_ = ValueType::Int;
    v.int_ = i;
    return v;
}
inline Value make_float(double d) {
    Value v = make_nil();
    v.type_ = ValueType::Float;
    v.float_ = d;
    return v;
}
inline Value make_obj(Obj* o) {
    Value v = make_nil();
    v.type_ = ValueType::Obj;
    v.obj_ = o;
    return v;
}

inline bool is_nil(Value v) { return v.type_ == ValueType::Nil; }
inline bool is_bool(Value v) { return v.type_ == ValueType::Bool; }
inline bool is_int(Value v) { return v.type_ == ValueType::Int; }
inline bool is_float(Value v) { return v.type_ == ValueType::Float; }
inline bool is_number(Value v) { return is_int(v) || is_float(v); }
inline bool is_obj(Value v) { return v.type_ == ValueType::Obj; }

inline bool as_bool(Value v) { return v.bool_; }
inline std::int32_t as_int(Value v) { return v.int_; }
inline double as_float(Value v) { return v.float_; }
inline Obj* as_obj(Value v) { return v.obj_; }

}  // namespace rung

// Typed helpers (is_string, as_array, is_obj_kind, ...) need the object types, which need
// Value. They live at the end of object.h; including either header gives you both.
#include "runtime/object.h"
