// Tests for the Value interface (notes D4, D15). Everything above the RUNG_NANBOX block runs
// against both implementations; the block below it pins the NaN-boxed bit layout.
#include <doctest.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/ops.h"
#include "runtime/value.h"

using namespace rung;

namespace {

double from_bits(std::uint64_t bits) {
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

std::uint64_t to_bits(double d) {
    std::uint64_t bits;
    std::memcpy(&bits, &d, sizeof bits);
    return bits;
}

std::string printed(Value v) {
    std::string out;
    print_value(v, out);
    return out;
}

// Exactly one type predicate holds for any Value.
int kinds(Value v) {
    return int{is_nil(v)} + int{is_bool(v)} + int{is_int(v)} + int{is_float(v)} + int{is_obj(v)};
}

// NaNs with assorted signs and payloads, including every pattern the NaN-boxed layout uses as a
// tag. Each must come back as an ordinary float NaN.
const std::vector<std::uint64_t> kNanBits = {
    0x7FF8'0000'0000'0000,  // the canonical quiet NaN
    0xFFF8'0000'0000'0000,  // x86's default NaN: sign bit set
    0x7FF0'0000'0000'0001,  // signalling NaN, smallest payload
    0x7FF4'0000'0000'0000,  // signalling NaN
    0x7FFC'0000'0000'0000,  // looks like the "not a double" prefix
    0x7FFD'0000'0000'002A,  // looks like the int 42
    0x7FFE'0000'0000'0000,  // looks like nil
    0x7FFF'0000'0000'0000,  // looks like false
    0x7FFF'0000'0000'0001,  // looks like true
    0xFFFC'0000'1234'5678,  // looks like a pointer
    0xFFFF'FFFF'FFFF'FFFF,  // every bit set
};

}  // namespace

TEST_CASE("Value: ints round-trip and are only ints") {
    for (std::int32_t i : {0, -1, 1, std::numeric_limits<std::int32_t>::min(),
                           std::numeric_limits<std::int32_t>::max(), 0x7FFF, -65536}) {
        Value v = make_int(i);
        CHECK(is_int(v));
        CHECK(is_number(v));
        CHECK(kinds(v) == 1);
        CHECK(as_int(v) == i);
    }
}

TEST_CASE("Value: doubles round-trip bit for bit") {
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<double> doubles = {
        0.0,      -0.0,     1.5,     -2.25,   inf,
        -inf,     DBL_MAX,  -DBL_MAX, DBL_MIN, std::numeric_limits<double>::denorm_min(),
        -std::numeric_limits<double>::denorm_min(), from_bits(0x000F'FFFF'FFFF'FFFF),  // subnormals
        2147483648.0, 1e300,
    };
    for (double d : doubles) {
        CAPTURE(d);
        Value v = make_float(d);
        CHECK(is_float(v));
        CHECK(is_number(v));
        CHECK(kinds(v) == 1);
        CHECK(to_bits(as_float(v)) == to_bits(d));  // keeps -0.0's sign, subnormal payloads
    }
    CHECK(printed(make_float(-0.0)) == "-0.0");
    CHECK(printed(make_float(inf)) == "inf");
    CHECK(printed(make_float(-inf)) == "-inf");
}

TEST_CASE("Value: every NaN comes back as a float NaN that prints nan") {
    for (std::uint64_t bits : kNanBits) {
        CAPTURE(bits);
        double d = from_bits(bits);
        REQUIRE(std::isnan(d));
        Value v = make_float(d);
        CHECK(is_float(v));
        CHECK(kinds(v) == 1);
        CHECK(std::isnan(as_float(v)));
        CHECK(printed(v) == "nan");
        CHECK_FALSE(values_equal(v, v));  // equality semantics live in ops and are unchanged
    }
    // NaNs made by arithmetic, not by bit patterns.
    const double inf = std::numeric_limits<double>::infinity();
    volatile double zero = 0.0;  // volatile so the NaN is produced at run time by the FPU
    for (double d : {zero / zero, inf - inf, inf * zero, -(zero / zero)}) {
        Value v = make_float(d);
        CHECK(is_float(v));
        CHECK(printed(v) == "nan");
    }
    CHECK(values_equal(make_float(-0.0), make_float(0.0)));
}

TEST_CASE("Value: nil and bools") {
    CHECK(is_nil(make_nil()));
    CHECK(kinds(make_nil()) == 1);
    CHECK(is_bool(make_bool(true)));
    CHECK(is_bool(make_bool(false)));
    CHECK(kinds(make_bool(true)) == 1);
    CHECK(kinds(make_bool(false)) == 1);
    CHECK(as_bool(make_bool(true)));
    CHECK_FALSE(as_bool(make_bool(false)));
    CHECK(printed(make_nil()) == "nil");
    CHECK(printed(make_bool(true)) == "true");
    CHECK(printed(make_bool(false)) == "false");
}

TEST_CASE("Value: real heap pointers round-trip") {
    Heap heap;
    std::vector<Obj*> objects;
    for (int i = 0; i < 64; ++i) {
        objects.push_back(heap.allocate<ObjArray>(static_cast<std::size_t>(i), make_int(i)));
    }
    objects.push_back(heap.intern(std::string_view("hello")));
    for (Obj* o : objects) {
        CHECK(pointer_fits_in_value(o));
        Value v = make_obj(o);
        CHECK(is_obj(v));
        CHECK(kinds(v) == 1);
        CHECK(as_obj(v) == o);
    }
    // Stack and static addresses fit too (ObjArray elements are on the C++ heap, these are not).
    int local = 0;
    CHECK(pointer_fits_in_value(&local));
    CHECK(pointer_fits_in_value(&kNanBits));
}

#if RUNG_NANBOX

TEST_CASE("NaN-boxed layout (notes D15)") {
    static_assert(sizeof(Value) == 8);

    CHECK(value_bits(make_nil()) == 0x7FFE'0000'0000'0000);
    CHECK(value_bits(make_bool(false)) == 0x7FFF'0000'0000'0000);
    CHECK(value_bits(make_bool(true)) == 0x7FFF'0000'0000'0001);
    CHECK(value_bits(make_int(0)) == 0x7FFD'0000'0000'0000);
    CHECK(value_bits(make_int(-1)) == 0x7FFD'0000'FFFF'FFFF);
    CHECK(value_bits(make_int(std::numeric_limits<std::int32_t>::min())) ==
          0x7FFD'0000'8000'0000);
    CHECK(value_bits(make_float(1.0)) == 0x3FF0'0000'0000'0000);
    CHECK(value_bits(make_float(-0.0)) == 0x8000'0000'0000'0000);

    // Every NaN, whatever its sign and payload, is stored as the one canonical pattern.
    for (std::uint64_t bits : kNanBits) {
        CAPTURE(bits);
        CHECK(value_bits(make_float(from_bits(bits))) == 0x7FF8'0000'0000'0000);
    }

    Heap heap;
    Obj* s = heap.intern(std::string_view("x"));
    std::uint64_t boxed = value_bits(make_obj(s));
    CHECK((boxed >> 48) == 0xFFFC);
    CHECK((boxed & 0x0000'FFFF'FFFF'FFFF) == reinterpret_cast<std::uintptr_t>(s));

    // A pointer with any of the top 16 bits set would be truncated, so it is rejected.
    CHECK_FALSE(pointer_fits_in_value(reinterpret_cast<const void*>(std::uintptr_t{1} << 48)));
}

#else

TEST_CASE("tagged-struct layout") { static_assert(sizeof(Value) == 16); }

#endif
