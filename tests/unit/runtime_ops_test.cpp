// Tests for the operations module and natives: every row of notes §2.1, §2.2 and §2.3, and every
// message in §2.5. Expected error text is spelled out literally (not taken from ops.cpp) so a
// wording change there fails here.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "diagnostic.h"
#include "runtime/heap.h"
#include "runtime/natives.h"
#include "runtime/object.h"
#include "runtime/ops.h"
#include "runtime/value.h"

using namespace rung;

namespace {

constexpr std::int32_t kMin = std::numeric_limits<std::int32_t>::min();
constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
const double kInf = std::numeric_limits<double>::infinity();
const double kNan = std::numeric_limits<double>::quiet_NaN();

Value I(std::int32_t i) { return make_int(i); }
Value F(double d) { return make_float(d); }
Value B(bool b) { return make_bool(b); }
Value N() { return make_nil(); }

Value str(Heap& heap, const char* text) { return make_obj(heap.intern(std::string_view(text))); }

Value arr(Heap& heap, std::vector<Value> elements) {
    return make_obj(heap.allocate<ObjArray>(std::move(elements)));
}

std::string printed(Value v) {
    std::string out;
    print_value(v, out);
    return out;
}

// One string per outcome: the printed result, or "ERR:" plus the message. Ints and floats print
// differently ("3" vs "3.0"), so comparing these strings also checks the result's type.
using BinaryFn = std::function<bool(Value, Value, Value*, std::string*)>;

std::string outcome(const BinaryFn& fn, Value a, Value b) {
    Value out = make_nil();
    std::string error;
    if (!fn(a, b, &out, &error)) return "ERR:" + error;
    return printed(out);
}

struct Case {
    std::string label;
    Value a;
    Value b;
    std::string want;
};

void run_cases(const BinaryFn& fn, const std::vector<Case>& cases) {
    for (const Case& c : cases) {
        INFO(c.label);
        CHECK(outcome(fn, c.a, c.b) == c.want);
    }
}

BinaryFn add_fn(Heap& heap) {
    return [&heap](Value a, Value b, Value* out, std::string* e) {
        return op_add(heap, a, b, out, e);
    };
}

const std::string kNumbers = "ERR:operands must be numbers";

}  // namespace

// ---- §2.1 arithmetic -------------------------------------------------------------------------

TEST_CASE("add: int wraparound, promotion, strings, errors") {
    Heap heap;
    Value hello = str(heap, "hello");
    Value world = str(heap, "world");
    Value empty = str(heap, "");
    run_cases(add_fn(heap),
              {
                  {"1 + 2", I(1), I(2), "3"},
                  {"INT_MAX + 1 wraps", I(kMax), I(1), "-2147483648"},
                  {"INT_MIN + -1 wraps", I(kMin), I(-1), "2147483647"},
                  {"int + float", I(1), F(2.5), "3.5"},
                  {"float + int", F(2.5), I(1), "3.5"},
                  {"float + float", F(0.5), F(0.25), "0.75"},
                  {"int + float is a float", I(1), F(2.0), "3.0"},
                  {"inf + -inf", F(kInf), F(-kInf), "nan"},
                  {"string + string", hello, world, "helloworld"},
                  {"empty + string", empty, hello, "hello"},
                  {"string + int", hello, I(1), "ERR:operands must be two numbers or two strings"},
                  {"int + string", I(1), hello, "ERR:operands must be two numbers or two strings"},
                  {"nil + nil", N(), N(), "ERR:operands must be two numbers or two strings"},
                  {"true + true", B(true), B(true),
                   "ERR:operands must be two numbers or two strings"},
                  {"array + array", arr(heap, {}), arr(heap, {}),
                   "ERR:operands must be two numbers or two strings"},
              });
}

TEST_CASE("add: concatenation yields the interned string") {
    Heap heap;
    Value out = make_nil();
    std::string error;
    REQUIRE(op_add(heap, str(heap, "ab"), str(heap, "cd"), &out, &error));
    // Same pointer as interning "abcd" directly, so == by identity is == by content.
    CHECK(as_obj(out) == as_obj(str(heap, "abcd")));
    CHECK(values_equal(out, str(heap, "abcd")));
}

TEST_CASE("add: safe when the collector runs on every allocation") {
    Heap heap;
    heap.set_stress(true);
    Value a = str(heap, "foo");
    Heap::TempRoot keep_a(heap, a);
    Value b = str(heap, "bar");
    Heap::TempRoot keep_b(heap, b);
    Value out = make_nil();
    std::string error;
    REQUIRE(op_add(heap, a, b, &out, &error));
    CHECK(printed(out) == "foobar");
}

TEST_CASE("subtract") {
    Heap heap;
    run_cases(
        [](Value a, Value b, Value* o, std::string* e) { return op_sub(a, b, o, e); },
        {
            {"5 - 3", I(5), I(3), "2"},
            {"INT_MIN - 1 wraps", I(kMin), I(1), "2147483647"},
            {"0 - INT_MIN wraps", I(0), I(kMin), "-2147483648"},
            {"int - float", I(5), F(0.5), "4.5"},
            {"float - int", F(0.5), I(1), "-0.5"},
            {"string - int", str(heap, "a"), I(1), kNumbers},
            {"int - true", I(1), B(true), kNumbers},
        });
}

TEST_CASE("multiply") {
    Heap heap;
    run_cases(
        [](Value a, Value b, Value* o, std::string* e) { return op_mul(a, b, o, e); },
        {
            {"6 * 7", I(6), I(7), "42"},
            {"INT_MAX * 2 wraps", I(kMax), I(2), "-2"},
            {"65536 * 65536 wraps to 0", I(65536), I(65536), "0"},
            {"INT_MIN * -1 wraps", I(kMin), I(-1), "-2147483648"},
            {"int * float", I(3), F(1.5), "4.5"},
            {"float * int", F(1.5), I(2), "3.0"},
            {"string * int", str(heap, "a"), I(2), kNumbers},
            {"nil * nil", N(), N(), kNumbers},
        });
}

TEST_CASE("divide") {
    Heap heap;
    run_cases(
        [](Value a, Value b, Value* o, std::string* e) { return op_div(a, b, o, e); },
        {
            {"7 / 2 truncates", I(7), I(2), "3"},
            {"-7 / 2 truncates toward zero", I(-7), I(2), "-3"},
            {"7 / -2 truncates toward zero", I(7), I(-2), "-3"},
            {"-7 / -2", I(-7), I(-2), "3"},
            {"INT_MIN / -1 wraps", I(kMin), I(-1), "-2147483648"},
            {"INT_MAX / -1", I(kMax), I(-1), "-2147483647"},
            {"INT_MIN / 1", I(kMin), I(1), "-2147483648"},
            {"5 / 0", I(5), I(0), "ERR:division by zero"},
            {"0 / 0", I(0), I(0), "ERR:division by zero"},
            {"5 / 0.0 is inf", I(5), F(0.0), "inf"},
            {"-5 / 0.0 is -inf", I(-5), F(0.0), "-inf"},
            {"5.0 / 0 is inf (promoted, no error)", F(5.0), I(0), "inf"},
            {"0 / 0.0 is nan", I(0), F(0.0), "nan"},
            {"1 / 2.0", I(1), F(2.0), "0.5"},
            {"7.0 / 2", F(7.0), I(2), "3.5"},
            {"string / int", str(heap, "a"), I(1), kNumbers},
            {"int / nil", I(1), N(), kNumbers},
        });
}

TEST_CASE("modulo") {
    Heap heap;
    const std::string ints = "ERR:operands of '%' must be ints";
    run_cases(
        [](Value a, Value b, Value* o, std::string* e) { return op_mod(a, b, o, e); },
        {
            {"7 % 3", I(7), I(3), "1"},
            {"-7 % 3 takes the sign of the left operand", I(-7), I(3), "-1"},
            {"7 % -3", I(7), I(-3), "1"},
            {"-7 % -3", I(-7), I(-3), "-1"},
            {"0 % 5", I(0), I(5), "0"},
            {"INT_MIN % -1 is 0", I(kMin), I(-1), "0"},
            {"INT_MAX % -1 is 0", I(kMax), I(-1), "0"},
            {"5 % 0", I(5), I(0), "ERR:division by zero"},
            {"float % int", F(5.0), I(2), ints},
            {"int % float", I(5), F(2.0), ints},
            {"float % float", F(5.0), F(2.0), ints},
            {"float % 0 is the type error, not division", F(5.0), I(0), ints},
            {"string % int", str(heap, "a"), I(1), ints},
            {"int % nil", I(1), N(), ints},
        });
}

TEST_CASE("unary minus and not") {
    Heap heap;
    struct Row {
        const char* label;
        Value in;
        std::string want;
    };
    std::vector<Row> rows = {
        {"-5", I(5), "-5"},
        {"-(-5)", I(-5), "5"},
        {"-0 stays 0", I(0), "0"},
        {"-(INT_MIN) wraps", I(kMin), "-2147483648"},
        {"-INT_MAX", I(kMax), "-2147483647"},
        {"-1.5", F(1.5), "-1.5"},
        {"-0.0 flips the sign", F(0.0), "-0.0"},
        {"-(-0.0)", F(-0.0), "0.0"},
        {"-nil", N(), "ERR:operand must be a number"},
        {"-true", B(true), "ERR:operand must be a number"},
        {"-string", str(heap, "a"), "ERR:operand must be a number"},
        {"-array", arr(heap, {}), "ERR:operand must be a number"},
    };
    for (const Row& r : rows) {
        INFO(r.label);
        Value out = make_nil();
        std::string error;
        std::string got = op_negate(r.in, &out, &error) ? printed(out) : "ERR:" + error;
        CHECK(got == r.want);
    }

    // `!` is truthiness inverted and never fails.
    CHECK(as_bool(op_not(N())));
    CHECK(as_bool(op_not(B(false))));
    CHECK_FALSE(as_bool(op_not(B(true))));
    CHECK_FALSE(as_bool(op_not(I(0))));
    CHECK_FALSE(as_bool(op_not(str(heap, ""))));
}

// ---- §2.1 comparison -------------------------------------------------------------------------

TEST_CASE("comparison: numbers only, int/float mixing allowed") {
    Heap heap;
    struct Op {
        const char* name;
        BinaryFn fn;
        // expected for (1,2), (2,1), (1,1)
        const char* lt_gt_eq[3];
    };
    std::vector<Op> ops = {
        {"<", [](Value a, Value b, Value* o, std::string* e) { return op_less(a, b, o, e); },
         {"true", "false", "false"}},
        {"<=", [](Value a, Value b, Value* o, std::string* e) { return op_less_equal(a, b, o, e); },
         {"true", "false", "true"}},
        {">", [](Value a, Value b, Value* o, std::string* e) { return op_greater(a, b, o, e); },
         {"false", "true", "false"}},
        {">=",
         [](Value a, Value b, Value* o, std::string* e) { return op_greater_equal(a, b, o, e); },
         {"false", "true", "true"}},
    };
    for (const Op& op : ops) {
        INFO(op.name);
        CHECK(outcome(op.fn, I(1), I(2)) == op.lt_gt_eq[0]);
        CHECK(outcome(op.fn, I(2), I(1)) == op.lt_gt_eq[1]);
        CHECK(outcome(op.fn, I(1), I(1)) == op.lt_gt_eq[2]);
        // The same relations hold across int and float, and among floats.
        CHECK(outcome(op.fn, I(1), F(2.0)) == op.lt_gt_eq[0]);
        CHECK(outcome(op.fn, F(2.0), I(1)) == op.lt_gt_eq[1]);
        CHECK(outcome(op.fn, I(1), F(1.0)) == op.lt_gt_eq[2]);
        CHECK(outcome(op.fn, F(1.0), F(1.0)) == op.lt_gt_eq[2]);
        // Extremes, and the sign of zero does not matter.
        CHECK(outcome(op.fn, I(kMin), I(kMax)) == op.lt_gt_eq[0]);
        CHECK(outcome(op.fn, F(-0.0), F(0.0)) == op.lt_gt_eq[2]);
        // NaN is unordered: every comparison is false.
        CHECK(outcome(op.fn, F(kNan), I(1)) == "false");
        CHECK(outcome(op.fn, I(1), F(kNan)) == "false");
        CHECK(outcome(op.fn, F(kNan), F(kNan)) == "false");
        // Non-numbers are errors, including strings compared with strings.
        CHECK(outcome(op.fn, str(heap, "a"), str(heap, "b")) == kNumbers);
        CHECK(outcome(op.fn, N(), I(1)) == kNumbers);
        CHECK(outcome(op.fn, I(1), B(true)) == kNumbers);
        CHECK(outcome(op.fn, arr(heap, {}), I(1)) == kNumbers);
    }
    CHECK(outcome(ops[0].fn, F(-kInf), F(kInf)) == "true");
}

// ---- §2.1 / §2.2 equality and truthiness -----------------------------------------------------

TEST_CASE("equality") {
    Heap heap;
    Value a1 = arr(heap, {I(1)});
    Value a2 = arr(heap, {I(1)});
    Value native = make_obj(heap.allocate<ObjNative>("clock", 0, native_clock));
    Value native2 = make_obj(heap.allocate<ObjNative>("clock", 0, native_clock));

    struct Row {
        const char* label;
        Value a;
        Value b;
        bool equal;
    };
    std::vector<Row> rows = {
        {"1 == 1", I(1), I(1), true},
        {"1 == 2", I(1), I(2), false},
        {"1 == 1.0", I(1), F(1.0), true},
        {"1.0 == 1", F(1.0), I(1), true},
        {"1 == 1.5", I(1), F(1.5), false},
        {"INT_MIN == INT_MIN", I(kMin), I(kMin), true},
        {"nan == nan is false", F(kNan), F(kNan), false},
        {"nan == 1 is false", F(kNan), I(1), false},
        {"-0.0 == 0.0", F(-0.0), F(0.0), true},
        {"-0.0 == 0", F(-0.0), I(0), true},
        {"inf == inf", F(kInf), F(kInf), true},
        {"inf == -inf", F(kInf), F(-kInf), false},
        {"nil == nil", N(), N(), true},
        {"nil == false", N(), B(false), false},
        {"true == true", B(true), B(true), true},
        {"true == false", B(true), B(false), false},
        {"false == false", B(false), B(false), true},
        {"0 == false (different types)", I(0), B(false), false},
        {"1 == true", I(1), B(true), false},
        {"0 == nil", I(0), N(), false},
        {"\"a\" == \"a\" by content", str(heap, "a"), str(heap, "a"), true},
        {"\"a\" == \"b\"", str(heap, "a"), str(heap, "b"), false},
        {"\"\" == \"\"", str(heap, ""), str(heap, ""), true},
        {"\"1\" == 1", str(heap, "1"), I(1), false},
        {"array == itself", a1, a1, true},
        {"array == equal-looking array (identity)", a1, a2, false},
        {"array == nil", a1, N(), false},
        {"native == itself", native, native, true},
        {"native == another native", native, native2, false},
    };
    for (const Row& r : rows) {
        INFO(r.label);
        CHECK(values_equal(r.a, r.b) == r.equal);
        CHECK(values_equal(r.b, r.a) == r.equal);  // symmetric
    }
}

TEST_CASE("truthiness: only false and nil are falsy") {
    Heap heap;
    CHECK_FALSE(is_truthy(N()));
    CHECK_FALSE(is_truthy(B(false)));
    CHECK(is_truthy(B(true)));
    CHECK(is_truthy(I(0)));
    CHECK(is_truthy(I(-1)));
    CHECK(is_truthy(F(0.0)));
    CHECK(is_truthy(F(kNan)));
    CHECK(is_truthy(str(heap, "")));
    CHECK(is_truthy(str(heap, "x")));
    CHECK(is_truthy(arr(heap, {})));
}

// ---- §2.3 printing ---------------------------------------------------------------------------

TEST_CASE("print: nil, bool, int, string") {
    Heap heap;
    CHECK(printed(N()) == "nil");
    CHECK(printed(B(true)) == "true");
    CHECK(printed(B(false)) == "false");
    CHECK(printed(I(0)) == "0");
    CHECK(printed(I(-42)) == "-42");
    CHECK(printed(I(kMin)) == "-2147483648");
    CHECK(printed(I(kMax)) == "2147483647");
    CHECK(printed(str(heap, "hi")) == "hi");  // raw, no quotes
    CHECK(printed(str(heap, "")) == "");
    CHECK(printed(str(heap, "a b\"c")) == "a b\"c");
}

TEST_CASE("print: floats") {
    struct Row {
        double x;
        const char* want;
    };
    std::vector<Row> rows = {
        {3.0, "3.0"},
        {0.1, "0.1"},
        {1e20, "1e+20"},
        {1.5e-7, "1.5e-07"},
        {-0.0, "-0.0"},
        {0.0, "0.0"},
        {kNan, "nan"},
        {-kNan, "nan"},  // never "-nan", whatever the sign bit says
        {std::copysign(kNan, -1.0), "nan"},
        {kInf, "inf"},
        {-kInf, "-inf"},
        {123456789.0, "123456789.0"},
        {1.5, "1.5"},
        {-1.5, "-1.5"},
        {2.5, "2.5"},
        {1234.5, "1234.5"},
        {0.5, "0.5"},
        // The §2.3 algorithm stops at the first precision that round-trips. For 100.0 that is
        // p = 1, which %g renders as "1e+02", and that already contains an 'e'.
        {100.0, "1e+02"},
        {1000000.0, "1e+06"},
        {0.1 + 0.2, "0.30000000000000004"},  // needs the full 17 digits
        {1.0 / 3.0, "0.3333333333333333"},
        {5e-324, "5e-324"},
        {1.7976931348623157e308, "1.7976931348623157e+308"},
        {-2147483648.0, "-2147483648.0"},
    };
    for (const Row& r : rows) {
        INFO(r.want);
        CHECK(printed(F(r.x)) == r.want);
    }
    // Round trip: whatever prints must parse back to the same double.
    for (double x : {0.1, 1.0 / 3.0, 12345.6789, 1e-10, 9007199254740993.0, 3.14159}) {
        CHECK(std::strtod(printed(F(x)).c_str(), nullptr) == x);
    }
    std::string appended = "x=";
    format_float(2.0, appended);
    CHECK(appended == "x=2.0");  // appends, doesn't overwrite
}

TEST_CASE("print: functions and natives") {
    Heap heap;
    Value native = make_obj(heap.allocate<ObjNative>("len", 1, native_len));
    CHECK(printed(native) == "<native fn>");
    // `<fn NAME>` for Rung functions arrives with the engine issues that add those object
    // kinds; print_object's switch has no default, so they cannot be forgotten.
}

TEST_CASE("print: arrays") {
    Heap heap;
    CHECK(printed(arr(heap, {})) == "[]");
    CHECK(printed(arr(heap, {I(1)})) == "[1]");
    CHECK(printed(arr(heap, {I(1), F(2.5), str(heap, "hi")})) == "[1, 2.5, hi]");
    CHECK(printed(arr(heap, {N(), B(true), B(false), F(3.0)})) == "[nil, true, false, 3.0]");
    CHECK(printed(arr(heap, {I(-1), F(kNan), F(-kInf)})) == "[-1, nan, -inf]");
    CHECK(printed(arr(heap, {str(heap, ""), str(heap, "")})) == "[, ]");

    Value inner = arr(heap, {I(1), I(2)});
    Value empty = arr(heap, {});
    Value nested = arr(heap, {inner, empty, arr(heap, {N(), arr(heap, {})})});
    CHECK(printed(nested) == "[[1, 2], [], [nil, []]]");

    // The same array twice is not a cycle: it must print in full both times.
    CHECK(printed(arr(heap, {inner, inner})) == "[[1, 2], [1, 2]]");

    Value native = make_obj(heap.allocate<ObjNative>("clock", 0, native_clock));
    CHECK(printed(arr(heap, {native})) == "[<native fn>]");
}

TEST_CASE("print: an array that contains itself prints [...] for the inner occurrence") {
    Heap heap;
    // a = [a]
    Value a = arr(heap, {N()});
    as_array(a)->elements[0] = a;
    CHECK(printed(a) == "[[...]]");

    // b = [1, b, 2]
    Value b = arr(heap, {I(1), N(), I(2)});
    as_array(b)->elements[1] = b;
    CHECK(printed(b) == "[1, [...], 2]");

    // Indirect: x = [y], y = [x]
    Value x = arr(heap, {N()});
    Value y = arr(heap, {x});
    as_array(x)->elements[0] = y;
    CHECK(printed(x) == "[[[...]]]");
    CHECK(printed(y) == "[[[...]]]");

    // A cycle beside a sibling that is a plain repeat: [c, c] where c = [c].
    Value c = arr(heap, {N()});
    as_array(c)->elements[0] = c;
    CHECK(printed(arr(heap, {c, c})) == "[[[...]], [[...]]]");
}

TEST_CASE("print: very deep nesting does not overflow the native stack") {
    Heap heap;
    constexpr int kDepth = 200000;
    Value v = arr(heap, {});
    // Keep the growing chain reachable: building it allocates past the collection threshold.
    heap.add_root_marker([&v](Heap& h) { h.mark_value(v); });
    for (int i = 0; i < kDepth; ++i) v = arr(heap, {v});
    std::string out = printed(v);
    CHECK(out.size() == static_cast<std::size_t>(2 * (kDepth + 1)));
    CHECK(out.front() == '[');
    CHECK(out.back() == ']');
}

// ---- §2.5 arrays -----------------------------------------------------------------------------

TEST_CASE("array_get and array_set") {
    Heap heap;
    Value a = arr(heap, {I(10), I(20), I(30)});
    std::string error;
    Value out = make_nil();

    REQUIRE(array_get(a, I(0), &out, &error));
    CHECK(printed(out) == "10");
    REQUIRE(array_get(a, I(2), &out, &error));
    CHECK(printed(out) == "30");

    REQUIRE(array_set(a, I(1), str(heap, "x"), &error));
    REQUIRE(array_get(a, I(1), &out, &error));
    CHECK(printed(out) == "x");
    CHECK(printed(a) == "[10, x, 30]");

    struct Row {
        const char* label;
        Value array;
        Value index;
        std::string want;
    };
    const std::string not_array = "can only index arrays";
    const std::string not_int = "array index must be an int";
    const std::string range = "array index out of range";
    std::vector<Row> rows = {
        {"index -1", a, I(-1), range},
        {"index len", a, I(3), range},
        {"index INT_MAX", a, I(kMax), range},
        {"index INT_MIN", a, I(kMin), range},
        {"float index", a, F(1.0), not_int},
        {"string index", a, str(heap, "0"), not_int},
        {"nil index", a, N(), not_int},
        {"bool index", a, B(true), not_int},
        {"index into an int", I(5), I(0), not_array},
        {"index into a string", str(heap, "abc"), I(0), not_array},
        {"index into nil", N(), I(0), not_array},
        {"not-array wins over bad index", I(5), str(heap, "x"), not_array},
        {"empty array, index 0", arr(heap, {}), I(0), range},
    };
    for (const Row& r : rows) {
        INFO(r.label);
        error.clear();
        Value untouched = I(99);
        out = untouched;
        CHECK_FALSE(array_get(r.array, r.index, &out, &error));
        CHECK(error == r.want);
        CHECK(values_equal(out, untouched));  // *out untouched on failure

        error.clear();
        CHECK_FALSE(array_set(r.array, r.index, I(1), &error));
        CHECK(error == r.want);
    }
    CHECK(printed(a) == "[10, x, 30]");  // failed sets changed nothing
}

TEST_CASE("arity_error_message") {
    CHECK(arity_error_message(1, 2) == "expected 1 arguments but got 2");
    CHECK(arity_error_message(0, 3) == "expected 0 arguments but got 3");
    CHECK(arity_error_message(2, 0) == "expected 2 arguments but got 0");
}

// ---- natives ---------------------------------------------------------------------------------

TEST_CASE("clock returns non-decreasing float seconds") {
    Heap heap;
    Value first = make_nil();
    Value second = make_nil();
    std::string error;
    REQUIRE(native_clock(heap, nullptr, &first, &error));
    REQUIRE(native_clock(heap, nullptr, &second, &error));
    CHECK(is_float(first));
    CHECK(is_float(second));
    CHECK(as_float(second) >= as_float(first));
}

TEST_CASE("len: arrays, strings (in bytes), and errors") {
    Heap heap;
    struct Row {
        const char* label;
        Value arg;
        std::string want;
    };
    std::vector<Row> rows = {
        {"empty array", arr(heap, {}), "0"},
        {"array of 3", arr(heap, {I(1), N(), F(2.0)}), "3"},
        {"empty string", str(heap, ""), "0"},
        {"string", str(heap, "hello"), "5"},
        {"multibyte string counts bytes", str(heap, "\xC3\xA9"), "2"},
        {"int", I(5), "ERR:len expects an array or a string"},
        {"float", F(1.0), "ERR:len expects an array or a string"},
        {"nil", N(), "ERR:len expects an array or a string"},
        {"bool", B(true), "ERR:len expects an array or a string"},
    };
    for (const Row& r : rows) {
        INFO(r.label);
        Value out = make_nil();
        std::string error;
        Value args[1] = {r.arg};
        std::string got = native_len(heap, args, &out, &error) ? printed(out) : "ERR:" + error;
        CHECK(got == r.want);
        if (got[0] != 'E') CHECK(is_int(out));
    }
}

TEST_CASE("array(n, fill)") {
    Heap heap;
    Value out = make_nil();
    std::string error;

    Value args[2] = {I(3), I(7)};
    REQUIRE(native_array(heap, args, &out, &error));
    CHECK(printed(out) == "[7, 7, 7]");

    args[0] = I(0);
    REQUIRE(native_array(heap, args, &out, &error));
    CHECK(printed(out) == "[]");

    // A fill that is an object is shared by reference, not copied.
    Value shared = arr(heap, {I(1)});
    Value args2[2] = {I(2), shared};
    REQUIRE(native_array(heap, args2, &out, &error));
    CHECK(as_obj(as_array(out)->elements[0]) == as_obj(shared));
    CHECK(as_obj(as_array(out)->elements[1]) == as_obj(shared));

    // Each call makes a distinct array.
    Value again = make_nil();
    REQUIRE(native_array(heap, args2, &again, &error));
    CHECK(as_obj(again) != as_obj(out));

    struct Row {
        const char* label;
        Value n;
    };
    std::vector<Row> bad = {
        {"negative", I(-1)},
        {"INT_MIN", I(kMin)},
        {"float", F(2.0)},
        {"string", str(heap, "3")},
        {"nil", N()},
        {"bool", B(true)},
    };
    for (const Row& r : bad) {
        INFO(r.label);
        Value bad_args[2] = {r.n, I(0)};
        Value untouched = I(99);
        out = untouched;
        error.clear();
        CHECK_FALSE(native_array(heap, bad_args, &out, &error));
        CHECK(error == "array size must be a non-negative int");
        CHECK(values_equal(out, untouched));
    }
}

TEST_CASE("array(n, fill) keeps its fill alive under GC stress") {
    Heap heap;
    heap.set_stress(true);
    Value fill = str(heap, "fill");  // deliberately not rooted by the test
    Value args[2] = {I(4), fill};
    Value out = make_nil();
    std::string error;
    REQUIRE(native_array(heap, args, &out, &error));
    Heap::TempRoot keep(heap, out);
    heap.collect();
    CHECK(printed(out) == "[fill, fill, fill, fill]");
}

TEST_CASE("register_natives defines clock, len and array with their arities") {
    Heap heap;
    std::vector<std::pair<std::string, Value>> table;
    heap.add_root_marker([&table](Heap& h) {
        for (auto& entry : table) h.mark_value(entry.second);
    });
    heap.set_stress(true);  // each definition must survive the allocations after it
    register_natives(heap, [&table](std::string_view name, Value native) {
        table.emplace_back(std::string(name), native);
    });
    REQUIRE(table.size() == 3);

    auto find = [&table](const std::string& name) -> ObjNative* {
        for (auto& entry : table) {
            if (entry.first == name) return as_native(entry.second);
        }
        return nullptr;
    };
    REQUIRE(find("clock") != nullptr);
    REQUIRE(find("len") != nullptr);
    REQUIRE(find("array") != nullptr);
    CHECK(find("clock")->arity == 0);
    CHECK(find("len")->arity == 1);
    CHECK(find("array")->arity == 2);
    CHECK(find("clock")->name == "clock");
    CHECK(printed(table[0].second) == "<native fn>");

    // And they actually work when called through the stored function pointer.
    Value out = make_nil();
    std::string error;
    Value args[1] = {str(heap, "abc")};
    Heap::TempRoot keep(heap, args[0]);
    REQUIRE(find("len")->function(heap, args, &out, &error));
    CHECK(printed(out) == "3");
}

// ---- diagnostic ------------------------------------------------------------------------------

TEST_CASE("runtime errors format as [line N] runtime error: MSG") {
    CHECK(format_runtime_error({7, "division by zero"}) ==
          "[line 7] runtime error: division by zero");
    CHECK(format_runtime_error({120, "can only call functions"}) ==
          "[line 120] runtime error: can only call functions");
    // Compile errors are formatted next to it and stay distinct.
    CHECK(format_error({3, "nesting too deep"}) == "[line 3] compile error: nesting too deep");
}
