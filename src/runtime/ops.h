#pragma once

#include <string>

#include "runtime/heap.h"
#include "runtime/value.h"

namespace rung {

// Every rule in docs/notes.md §2.1-2.5, implemented exactly once (notes D10). The tree-walker,
// both VMs and the JIT's slow paths call these; none of them reimplements an arithmetic,
// comparison, printing or error-message rule. (The JIT inlines the int fast paths only.)
//
// Fallible operations return true on success, writing `*out`. On failure they return false and
// write the exact §2.5 message to `*error`, leaving `*out` untouched. They never know about
// line numbers: the engine attaches those (see RuntimeError in diagnostic.h).
//
// GC contract: only op_add on two strings allocates. It copies both operands' bytes before it
// allocates, so the operands may safely die during that collection, but callers should still
// keep both rooted (on the engine's stack or in a Heap::TempRoot) until the call returns. The
// result is NOT rooted: store it somewhere the collector can see before the next allocation.

// ---- Binary arithmetic (§2.1) ----------------------------------------------------------------
// int (+ - *) int wraps in 32 bits. int / int truncates toward zero; INT32_MIN / -1 is
// INT32_MIN; INT32_MIN % -1 is 0; % takes the sign of the left operand; int / or % by zero is
// "division by zero". An int mixed with a float promotes to float. Float / 0.0 follows IEEE.
// % with any non-int operand is "operands of '%' must be ints". `+` also concatenates two
// strings into a new interned string; `+` with any other operand mix is
// "operands must be two numbers or two strings". - * / with a non-number operand is
// "operands must be numbers".
bool op_add(Heap& heap, Value a, Value b, Value* out, std::string* error);
bool op_sub(Value a, Value b, Value* out, std::string* error);
bool op_mul(Value a, Value b, Value* out, std::string* error);
bool op_div(Value a, Value b, Value* out, std::string* error);
bool op_mod(Value a, Value b, Value* out, std::string* error);

// ---- Unary -----------------------------------------------------------------------------------
// Ints wrap: -(-2147483648) is -2147483648. Non-number: "operand must be a number".
bool op_negate(Value a, Value* out, std::string* error);
// `!` never fails: it is truthiness, inverted.
Value op_not(Value a);

// ---- Comparison (§2.1) -----------------------------------------------------------------------
// < <= > >= on numbers only (int and float may be mixed). Anything else is
// "operands must be numbers". The result is a Bool value.
bool op_less(Value a, Value b, Value* out, std::string* error);
bool op_less_equal(Value a, Value b, Value* out, std::string* error);
bool op_greater(Value a, Value b, Value* out, std::string* error);
bool op_greater_equal(Value a, Value b, Value* out, std::string* error);

// ---- Equality and truthiness (§2.1, §2.2) ----------------------------------------------------
// Numbers compare numerically across int and float (1 == 1.0; NaN != NaN; -0.0 == 0.0).
// Strings compare by content, which is pointer equality because every string is interned.
// Arrays and functions compare by identity. Values of different types are never equal.
bool values_equal(Value a, Value b);
// Only `false` and `nil` are falsy; 0, 0.0, "" and [] are truthy.
inline bool is_truthy(Value v) { return !(is_nil(v) || (is_bool(v) && !as_bool(v))); }

// ---- Printing (§2.3) -------------------------------------------------------------------------
// Appends the printed form of `v` to `out` (no trailing newline). Byte-identical on every
// engine and platform. Arrays are printed iteratively with an explicit "currently printing"
// set, so an array that contains itself prints the inner occurrence as `[...]` and a very
// deeply nested array cannot overflow the native stack.
//
// Extending for new object kinds: print_object in ops.cpp is a switch over every ObjKind with
// no `default`, so adding a kind (TreeFunction, Function, Closure, ...) is a -Werror -Wswitch
// error until you give it a case there (functions print as `<fn NAME>`).
void print_value(Value v, std::string& out);
// The float rule from §2.3 on its own: `nan`, `inf`, `-inf`, or the shortest `%.*g` (p in
// 1..17) that round-trips through strtod, with `.0` appended if it has no `.` or `e`.
void format_float(double x, std::string& out);

// ---- Arrays (§2.5) ---------------------------------------------------------------------------
// `array` must be an array ("can only index arrays"), `index` an int ("array index must be an
// int"), and in [0, len) ("array index out of range"), checked in that order.
bool array_get(Value array, Value index, Value* out, std::string* error);
bool array_set(Value array, Value index, Value value, std::string* error);

// ---- Shared error text -----------------------------------------------------------------------
// "expected A arguments but got B". Callers check arity before calling a Rung or native
// function, so that every engine (and every native) words it identically.
std::string arity_error_message(int expected, int got);

}  // namespace rung
