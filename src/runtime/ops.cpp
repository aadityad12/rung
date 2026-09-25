#include "runtime/ops.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "runtime/function.h"

namespace rung {

namespace {

// ---- Wrapping integer math (notes D1, D10) ---------------------------------------------------
// Signed overflow is undefined behaviour in C++, and the optimiser is allowed to assume it never
// happens. Unsigned arithmetic is defined to wrap modulo 2^32, so we do the math there and
// convert back. Since C++20 the unsigned-to-signed conversion is defined as two's complement,
// which is exactly what the ARM64 W registers do, so JIT'd code will agree bit for bit.
std::int32_t wrap_add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t wrap_sub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
std::int32_t wrap_mul(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
std::int32_t wrap_neg(std::int32_t a) {
    return static_cast<std::int32_t>(std::uint32_t{0} - static_cast<std::uint32_t>(a));
}

double to_double(Value v) { return is_int(v) ? static_cast<double>(as_int(v)) : as_float(v); }

// ---- Error messages (notes §2.5). Spelled out here, in one place. ----------------------------
constexpr const char* kErrAddOperands = "operands must be two numbers or two strings";
constexpr const char* kErrNumbers = "operands must be numbers";
constexpr const char* kErrModInts = "operands of '%' must be ints";
constexpr const char* kErrNegate = "operand must be a number";
constexpr const char* kErrDivZero = "division by zero";
constexpr const char* kErrIndexNotArray = "can only index arrays";
constexpr const char* kErrIndexNotInt = "array index must be an int";
constexpr const char* kErrIndexRange = "array index out of range";

// Shared shape of - * /: both ints takes the int path, any other number mix promotes to double,
// a non-number is an error.
template <class IntFn, class FloatFn>
bool numeric_binary(Value a, Value b, Value* out, std::string* error, IntFn int_fn,
                    FloatFn float_fn) {
    if (!is_number(a) || !is_number(b)) {
        *error = kErrNumbers;
        return false;
    }
    if (is_int(a) && is_int(b)) return int_fn(as_int(a), as_int(b), out, error);
    *out = make_float(float_fn(to_double(a), to_double(b)));
    return true;
}

// Shared shape of < <= > >=: numbers only, ints compared as ints, otherwise as doubles (an
// int32 converts to double exactly, so mixed comparison loses nothing). NaN compares false.
template <class IntCmp, class FloatCmp>
bool numeric_compare(Value a, Value b, Value* out, std::string* error, IntCmp int_cmp,
                     FloatCmp float_cmp) {
    if (!is_number(a) || !is_number(b)) {
        *error = kErrNumbers;
        return false;
    }
    if (is_int(a) && is_int(b)) {
        *out = make_bool(int_cmp(as_int(a), as_int(b)));
    } else {
        *out = make_bool(float_cmp(to_double(a), to_double(b)));
    }
    return true;
}

// Appends a non-array value. Arrays are handled by print_value's explicit stack.
// `<fn NAME>` (notes §2.3). Only the top-level script has no name; it is never printed by a
// program, but the disassembler and debugging output can show it.
std::string function_label(const ObjFunction& fn) {
    return fn.name != nullptr ? "<fn " + fn.name->chars + ">" : "<script>";
}

void print_object(const Obj& obj, std::string& out) {
    // No `default`: a new ObjKind must be handled here or the build fails (-Wswitch, -Werror).
    switch (obj.kind) {
        case ObjKind::String:
            out += static_cast<const ObjString&>(obj).chars;
            return;
        case ObjKind::Native:
            out += "<native fn>";  // notes §2.3
            return;
        case ObjKind::Array:
            out += "[...]";  // unreachable from print_value; a safe answer if ever called directly
            return;
        case ObjKind::Function:
            out += function_label(static_cast<const ObjFunction&>(obj));
            return;
        case ObjKind::Closure:  // a closure is what the program sees as "a function"
            out += function_label(*static_cast<const ObjClosure&>(obj).function);
            return;
        case ObjKind::Upvalue:
            out += "<upvalue>";  // never a program-visible value; the VM keeps them out of slots
            return;
    }
}

void print_scalar(Value v, std::string& out) {
    if (is_nil(v)) {
        out += "nil";
    } else if (is_bool(v)) {
        out += as_bool(v) ? "true" : "false";
    } else if (is_int(v)) {
        out += std::to_string(as_int(v));
    } else if (is_float(v)) {
        format_float(as_float(v), out);
    } else {
        print_object(*as_obj(v), out);
    }
}

}  // namespace

// ---- Arithmetic ------------------------------------------------------------------------------

bool op_add(Heap& heap, Value a, Value b, Value* out, std::string* error) {
    if (is_string(a) && is_string(b)) {
        // Build the bytes first: the intern() below may collect, and by then the result no
        // longer depends on either operand object being alive.
        std::string joined;
        joined.reserve(as_string(a)->chars.size() + as_string(b)->chars.size());
        joined += as_string(a)->chars;
        joined += as_string(b)->chars;
        *out = make_obj(heap.intern(std::move(joined)));
        return true;
    }
    if (is_number(a) && is_number(b)) {
        if (is_int(a) && is_int(b)) {
            *out = make_int(wrap_add(as_int(a), as_int(b)));
        } else {
            *out = make_float(to_double(a) + to_double(b));
        }
        return true;
    }
    *error = kErrAddOperands;
    return false;
}

bool op_sub(Value a, Value b, Value* out, std::string* error) {
    return numeric_binary(
        a, b, out, error,
        [](std::int32_t x, std::int32_t y, Value* o, std::string*) {
            *o = make_int(wrap_sub(x, y));
            return true;
        },
        [](double x, double y) { return x - y; });
}

bool op_mul(Value a, Value b, Value* out, std::string* error) {
    return numeric_binary(
        a, b, out, error,
        [](std::int32_t x, std::int32_t y, Value* o, std::string*) {
            *o = make_int(wrap_mul(x, y));
            return true;
        },
        [](double x, double y) { return x * y; });
}

bool op_div(Value a, Value b, Value* out, std::string* error) {
    return numeric_binary(
        a, b, out, error,
        [](std::int32_t x, std::int32_t y, Value* o, std::string* e) {
            if (y == 0) {
                *e = kErrDivZero;  // ARM64 SDIV would silently give 0; the JIT must check too
                return false;
            }
            // INT32_MIN / -1 does not fit (and traps on x86, is UB in C++). The wrapped answer
            // is INT32_MIN, which is also what ARM64 SDIV returns. Any x / -1 is just -x.
            *o = make_int(y == -1 ? wrap_neg(x) : x / y);  // C++ truncates toward zero
            return true;
        },
        [](double x, double y) { return x / y; });  // IEEE: 1.0 / 0.0 is inf, no error
}

bool op_mod(Value a, Value b, Value* out, std::string* error) {
    // A float operand and a non-number operand get the same message (§2.5).
    if (!is_int(a) || !is_int(b)) {
        *error = kErrModInts;
        return false;
    }
    std::int32_t x = as_int(a);
    std::int32_t y = as_int(b);
    if (y == 0) {
        *error = kErrDivZero;
        return false;
    }
    // INT32_MIN % -1 is UB in C++ even though the mathematical answer is 0; so is any x % -1.
    // Otherwise C++ gives the remainder the sign of the left operand, as §2.1 requires.
    *out = make_int(y == -1 ? 0 : x % y);
    return true;
}

bool op_negate(Value a, Value* out, std::string* error) {
    if (is_int(a)) {
        *out = make_int(wrap_neg(as_int(a)));
        return true;
    }
    if (is_float(a)) {
        *out = make_float(-as_float(a));
        return true;
    }
    *error = kErrNegate;
    return false;
}

Value op_not(Value a) { return make_bool(!is_truthy(a)); }

// ---- Comparison and equality -----------------------------------------------------------------

bool op_less(Value a, Value b, Value* out, std::string* error) {
    return numeric_compare(
        a, b, out, error, [](std::int32_t x, std::int32_t y) { return x < y; },
        [](double x, double y) { return x < y; });
}
bool op_less_equal(Value a, Value b, Value* out, std::string* error) {
    return numeric_compare(
        a, b, out, error, [](std::int32_t x, std::int32_t y) { return x <= y; },
        [](double x, double y) { return x <= y; });
}
bool op_greater(Value a, Value b, Value* out, std::string* error) {
    return numeric_compare(
        a, b, out, error, [](std::int32_t x, std::int32_t y) { return x > y; },
        [](double x, double y) { return x > y; });
}
bool op_greater_equal(Value a, Value b, Value* out, std::string* error) {
    return numeric_compare(
        a, b, out, error, [](std::int32_t x, std::int32_t y) { return x >= y; },
        [](double x, double y) { return x >= y; });
}

bool values_equal(Value a, Value b) {
    if (is_number(a) && is_number(b)) {
        if (is_int(a) && is_int(b)) return as_int(a) == as_int(b);
        return to_double(a) == to_double(b);  // NaN != NaN and -0.0 == 0.0 fall out of IEEE ==
    }
    if (is_nil(a) && is_nil(b)) return true;
    if (is_bool(a) && is_bool(b)) return as_bool(a) == as_bool(b);
    // Strings are interned, so identity is content equality; arrays and functions are equal
    // only to themselves (notes §2.2).
    if (is_obj(a) && is_obj(b)) return as_obj(a) == as_obj(b);
    return false;  // different types
}

// ---- Printing --------------------------------------------------------------------------------

void format_float(double x, std::string& out) {
    // Any NaN prints "nan": the sign bit of a NaN is not meaningful and printf may show "-nan".
    if (std::isnan(x)) {
        out += "nan";
        return;
    }
    if (std::isinf(x)) {
        out += x < 0 ? "-inf" : "inf";
        return;
    }
    // The shortest %g that reads back as exactly x. 17 significant digits always round-trips a
    // double, so the loop always ends by p = 17. std::to_chars was rejected (notes §2.3): its
    // shortest form is not %g-shaped, and older Apple libc++ gates it by OS version.
    char buf[40];
    for (int p = 1; p <= 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, x);
        if (std::strtod(buf, nullptr) == x) break;
    }
    out += buf;
    if (std::strpbrk(buf, ".e") == nullptr) out += ".0";
}

void print_value(Value v, std::string& out) {
    if (!is_array(v)) {
        print_scalar(v, out);
        return;
    }

    // An explicit stack instead of recursion, so a very deep array (built by a loop) cannot
    // overflow the native stack. `printing` holds exactly the arrays whose `[` is written but
    // whose `]` is not: meeting one of those again means a cycle.
    struct Frame {
        const ObjArray* array;
        std::size_t next;
    };
    std::vector<Frame> stack;
    std::unordered_set<const ObjArray*> printing;

    auto open = [&](const ObjArray* array) {
        if (printing.count(array) != 0) {
            out += "[...]";
            return;
        }
        out += '[';
        printing.insert(array);
        stack.push_back({array, 0});
    };

    open(as_array(v));
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.next == frame.array->elements.size()) {
            out += ']';
            printing.erase(frame.array);
            stack.pop_back();
            continue;
        }
        if (frame.next > 0) out += ", ";
        Value element = frame.array->elements[frame.next++];
        if (is_array(element)) {
            open(as_array(element));  // may invalidate `frame`; not used again this iteration
        } else {
            print_scalar(element, out);
        }
    }
}

// ---- Arrays ----------------------------------------------------------------------------------

namespace {

// Validates array + index and yields the element position.
bool resolve_index(Value array, Value index, std::size_t* position, std::string* error) {
    if (!is_array(array)) {
        *error = kErrIndexNotArray;
        return false;
    }
    if (!is_int(index)) {
        *error = kErrIndexNotInt;
        return false;
    }
    std::int32_t i = as_int(index);
    if (i < 0 || static_cast<std::size_t>(i) >= as_array(array)->elements.size()) {
        *error = kErrIndexRange;
        return false;
    }
    *position = static_cast<std::size_t>(i);
    return true;
}

}  // namespace

bool array_get(Value array, Value index, Value* out, std::string* error) {
    std::size_t position;
    if (!resolve_index(array, index, &position, error)) return false;
    *out = as_array(array)->elements[position];
    return true;
}

bool array_set(Value array, Value index, Value value, std::string* error) {
    std::size_t position;
    if (!resolve_index(array, index, &position, error)) return false;
    as_array(array)->elements[position] = value;
    return true;
}

std::string arity_error_message(int expected, int got) {
    return "expected " + std::to_string(expected) + " arguments but got " + std::to_string(got);
}

}  // namespace rung
