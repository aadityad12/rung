#pragma once

#include <cstdint>

#if RUNG_NANBOX
#include <cmath>
#include <cstring>
#endif

namespace rung {

struct Obj;

// Two implementations of one interface (notes D4): the free functions below are the only way
// any code outside this header creates, tests or reads a Value. RUNG_NANBOX picks which one is
// compiled; nothing else in the program changes.

#if !RUNG_NANBOX

// The tagged-struct Value: a 1-byte type tag plus an 8-byte union. Alignment pads the tag out
// to 8 bytes, so the whole thing is 16 bytes (notes D4, D10).
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

// Any pointer fits in the tagged struct.
inline bool pointer_fits_in_value(const void*) { return true; }

#else  // RUNG_NANBOX

// The NaN-boxed Value: every value in one 64-bit word (notes D15 has the layout drawn out).
// This follows clox's NaN boxing (Crafting Interpreters, "Optimization") closely: the same
// quiet-NaN prefix and the sign bit marking a pointer. clox has no ints; the int32 tag is ours.
//
// A double whose 11 exponent bits are all ones and whose 52 mantissa bits are not all zero is a
// NaN, and IEEE arithmetic never looks inside the mantissa of one. So every bit pattern with
// bits 50..62 set (exponent all ones, quiet bit 51 set, and bit 50 set as well) is a NaN that
// we reserve for non-doubles. Real NaNs are folded to kCanonicalNan (bit 50 clear) when boxed,
// so no arithmetic result can ever look like a tag.
//
//   double     any bit pattern except a NaN; NaNs become 0x7FF8'0000'0000'0000
//   int32      0x7FFD'0000'iiii'iiii            (low 32 bits = the int)
//   nil        0x7FFE'0000'0000'0000
//   false      0x7FFF'0000'0000'0000
//   true       0x7FFF'0000'0000'0001
//   Obj*       0xFFFC'pppp'pppp'pppp            (sign bit set, pointer in the low 48 bits)
struct Value {
    // Private by convention: use the free functions only.
    std::uint64_t bits_;
};

static_assert(sizeof(Value) == 8, "the NaN-boxed Value is 8 bytes (notes D4, D15)");

namespace nanbox {
inline constexpr std::uint64_t kQnan = 0x7FFC'0000'0000'0000;  // bits 50..62: "not a double"
inline constexpr std::uint64_t kSign = 0x8000'0000'0000'0000;
inline constexpr std::uint64_t kCanonicalNan = 0x7FF8'0000'0000'0000;
inline constexpr std::uint64_t kTagMask = 0xFFFF'0000'0000'0000;  // sign, exponent, bits 48..51
inline constexpr std::uint64_t kIntTag = kQnan | 0x0001'0000'0000'0000;
inline constexpr std::uint64_t kNil = kQnan | 0x0002'0000'0000'0000;
inline constexpr std::uint64_t kFalse = kQnan | 0x0003'0000'0000'0000;
inline constexpr std::uint64_t kTrue = kFalse | 1;
inline constexpr std::uint64_t kObjTag = kSign | kQnan;
inline constexpr std::uint64_t kPointerMask = 0x0000'FFFF'FFFF'FFFF;
// An int's high word must be exactly the tag: bits 32..47 are always zero.
inline constexpr std::uint64_t kIntMask = 0xFFFF'FFFF'0000'0000;
}  // namespace nanbox

inline Value make_nil() { return Value{nanbox::kNil}; }
inline Value make_bool(bool b) { return Value{b ? nanbox::kTrue : nanbox::kFalse}; }
inline Value make_int(std::int32_t i) {
    // Through uint32_t so a negative int fills only the low 32 bits, not the tag (notes D1).
    return Value{nanbox::kIntTag | static_cast<std::uint32_t>(i)};
}
inline Value make_float(double d) {
    // A NaN from arithmetic may carry any sign and payload (x86 produces 0xFFF8..., an operand's
    // payload can propagate), and some of those patterns are our tags. Fold every NaN to the
    // one canonical pattern, which is outside the tag space. Printing and equality do not care
    // which NaN it was (notes §2.1, §2.3).
    if (std::isnan(d)) return Value{nanbox::kCanonicalNan};
    std::uint64_t bits;
    std::memcpy(&bits, &d, sizeof bits);
    return Value{bits};
}
inline Value make_obj(Obj* o) {
    // Heap::link checks pointer_fits_in_value for every object, so the mask loses nothing.
    return Value{nanbox::kObjTag | (reinterpret_cast<std::uintptr_t>(o) & nanbox::kPointerMask)};
}

inline bool is_nil(Value v) { return v.bits_ == nanbox::kNil; }
inline bool is_bool(Value v) { return (v.bits_ | 1) == nanbox::kTrue; }
inline bool is_int(Value v) { return (v.bits_ & nanbox::kIntMask) == nanbox::kIntTag; }
// Every non-double has bits 50..62 set; no double does once NaNs are canonical.
inline bool is_float(Value v) { return (v.bits_ & nanbox::kQnan) != nanbox::kQnan; }
inline bool is_number(Value v) { return is_int(v) || is_float(v); }
inline bool is_obj(Value v) { return (v.bits_ & nanbox::kTagMask) == nanbox::kObjTag; }

inline bool as_bool(Value v) { return v.bits_ == nanbox::kTrue; }
inline std::int32_t as_int(Value v) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(v.bits_));
}
inline double as_float(Value v) {
    double d;
    std::memcpy(&d, &v.bits_, sizeof d);
    return d;
}
inline Obj* as_obj(Value v) {
    return reinterpret_cast<Obj*>(static_cast<std::uintptr_t>(v.bits_ & nanbox::kPointerMask));
}

// User-space pointers on arm64 macOS and Linux (and x86-64 Linux) use at most 48 bits, with the
// top 16 zero. A pointer that did not would lose its high bits in make_obj.
inline bool pointer_fits_in_value(const void* p) {
    return (reinterpret_cast<std::uintptr_t>(p) & ~nanbox::kPointerMask) == 0;
}

// The raw word, for tests that check the layout. Engines never look at it.
inline std::uint64_t value_bits(Value v) { return v.bits_; }

#endif  // RUNG_NANBOX

}  // namespace rung

// Typed helpers (is_string, as_array, is_obj_kind, ...) need the object types, which need
// Value. They live at the end of object.h; including either header gives you both.
#include "runtime/object.h"
