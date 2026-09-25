#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/value.h"

using namespace rung;

namespace {

Value str(Heap& heap, const char* text) { return make_obj(heap.intern(std::string_view(text))); }

}  // namespace

TEST_CASE("Value round-trips every type") {
    CHECK(is_nil(make_nil()));
    CHECK_FALSE(is_number(make_nil()));

    CHECK(as_bool(make_bool(true)));
    CHECK_FALSE(as_bool(make_bool(false)));
    CHECK(is_bool(make_bool(false)));
    CHECK_FALSE(is_nil(make_bool(false)));

    for (std::int32_t i : {0, 1, -1, 42, std::numeric_limits<std::int32_t>::max(),
                           std::numeric_limits<std::int32_t>::min()}) {
        Value v = make_int(i);
        CHECK(is_int(v));
        CHECK(is_number(v));
        CHECK_FALSE(is_float(v));
        CHECK(as_int(v) == i);
    }

    Value neg_zero = make_float(-0.0);
    CHECK(is_float(neg_zero));
    CHECK(is_number(neg_zero));
    CHECK_FALSE(is_int(neg_zero));
    CHECK(as_float(neg_zero) == 0.0);
    CHECK(std::signbit(as_float(neg_zero)));

    double inf = std::numeric_limits<double>::infinity();
    CHECK(as_float(make_float(inf)) == inf);
    CHECK(as_float(make_float(-inf)) == -inf);
    CHECK(std::isnan(as_float(make_float(std::numeric_limits<double>::quiet_NaN()))));
    CHECK(as_float(make_float(1.5)) == 1.5);

    Heap heap;
    Value s = str(heap, "x");
    CHECK(is_obj(s));
    CHECK_FALSE(is_number(s));
    CHECK(as_obj(s) == static_cast<Obj*>(as_string(s)));
}

TEST_CASE("typed object helpers") {
    Heap heap;
    Value s = str(heap, "hi");
    Value a = make_obj(heap.allocate<ObjArray>(std::size_t{2}, make_nil()));
    CHECK(is_string(s));
    CHECK_FALSE(is_array(s));
    CHECK(is_array(a));
    CHECK(is_obj_kind(a, ObjKind::Array));
    CHECK_FALSE(is_obj_kind(make_int(1), ObjKind::Array));
    CHECK(as_array(a)->elements.size() == 2);
    CHECK(as_string(s)->chars == "hi");
    CHECK(as_string(s)->hash == fnv1a("hi"));
}

TEST_CASE("fnv1a matches known vectors") {
    CHECK(fnv1a("") == 2166136261u);
    CHECK(fnv1a("a") == 0xe40c292cu);
    CHECK(fnv1a("foobar") == 0xbf9cf968u);
}

TEST_CASE("interning") {
    Heap heap;
    ObjString* a = heap.intern(std::string_view("abc"));
    ObjString* b = heap.intern(std::string_view("abc"));
    ObjString* c = heap.intern(std::string("abc"));
    ObjString* d = heap.intern(std::string_view("abd"));
    CHECK(a == b);
    CHECK(a == c);
    CHECK(a != d);
    CHECK(heap.stats().live_objects == 2);
    CHECK(heap.intern(std::string_view("")) == heap.intern(std::string("")));
}

TEST_CASE("root markers keep objects alive; unreachable ones are freed") {
    Heap heap;
    Value kept = str(heap, "kept");
    str(heap, "garbage");
    Heap::RootHandle handle = heap.add_root_marker([&](Heap& h) { h.mark_value(kept); });
    CHECK(heap.stats().live_objects == 2);

    heap.collect();
    CHECK(heap.stats().live_objects == 1);
    CHECK(heap.stats().collections == 1);
    CHECK(as_string(kept)->chars == "kept");
    CHECK(heap.intern(std::string_view("kept")) == as_string(kept));

    heap.remove_root_marker(handle);
    heap.collect();
    CHECK(heap.stats().live_objects == 0);
    CHECK(heap.stats().live_bytes == 0);
}

TEST_CASE("TempRoot protects a local across a collection") {
    Heap heap;
    {
        Value v = str(heap, "temp");
        Heap::TempRoot guard(heap, v);
        heap.collect();
        CHECK(heap.stats().live_objects == 1);
        CHECK(as_string(v)->chars == "temp");
    }
    heap.collect();
    CHECK(heap.stats().live_objects == 0);
}

TEST_CASE("nested TempRoots unwind in LIFO order") {
    Heap heap;
    Value a = str(heap, "a");
    Value b = str(heap, "b");
    {
        Heap::TempRoot ra(heap, a);
        {
            Heap::TempRoot rb(heap, b);
            heap.collect();
            CHECK(heap.stats().live_objects == 2);
        }
        heap.collect();
        CHECK(heap.stats().live_objects == 1);
    }
}

TEST_CASE("arrays keep their elements alive") {
    Heap heap;
    Value arr = make_obj(heap.allocate<ObjArray>(std::size_t{3}, make_nil()));
    Heap::RootHandle handle = heap.add_root_marker([&](Heap& h) { h.mark_value(arr); });
    as_array(arr)->elements[0] = str(heap, "one");
    as_array(arr)->elements[1] = make_int(2);
    as_array(arr)->elements[2] = make_obj(heap.allocate<ObjArray>(std::size_t{1}, make_nil()));
    str(heap, "garbage");

    heap.collect();
    CHECK(heap.stats().live_objects == 3);  // outer array, "one", inner array
    CHECK(as_string(as_array(arr)->elements[0])->chars == "one");
    heap.remove_root_marker(handle);
}

TEST_CASE("an array containing itself does not hang the marker") {
    Heap heap;
    Value arr = make_obj(heap.allocate<ObjArray>(std::size_t{1}, make_nil()));
    as_array(arr)->elements[0] = arr;  // a direct self-cycle
    {
        Heap::TempRoot guard(heap, arr);
        heap.collect();
        CHECK(heap.stats().live_objects == 1);
    }
    Value other = make_obj(heap.allocate<ObjArray>(std::size_t{1}, arr));
    as_array(arr)->elements[0] = other;  // arr -> other -> arr, an indirect cycle

    {
        Heap::TempRoot guard(heap, arr);
        heap.collect();
        CHECK(heap.stats().live_objects == 2);
    }
    heap.collect();  // an unreachable cycle is freed too (mark-sweep, not refcounting)
    CHECK(heap.stats().live_objects == 0);
}

TEST_CASE("deeply nested arrays do not overflow the native stack") {
    Heap heap;
    Value inner = make_obj(heap.allocate<ObjArray>(std::size_t{1}, make_nil()));
    Value root = inner;
    Heap::TempRoot guard(heap, root);
    for (int i = 0; i < 200000; ++i) {
        Value next = make_obj(heap.allocate<ObjArray>(std::size_t{1}, make_nil()));
        as_array(inner)->elements[0] = next;
        inner = next;
    }
    heap.collect();
    CHECK(heap.stats().live_objects == 200001);
}

TEST_CASE("a dead string leaves the intern table") {
    Heap heap;
    ObjString* first = heap.intern(std::string_view("ephemeral"));
    (void)first;  // do not compare the pointer after it is freed
    heap.collect();
    CHECK(heap.stats().live_objects == 0);

    ObjString* second = heap.intern(std::string_view("ephemeral"));
    CHECK(second->chars == "ephemeral");
    CHECK(heap.stats().live_objects == 1);
    CHECK(heap.intern(std::string_view("ephemeral")) == second);
}

TEST_CASE("natives allocate and carry their metadata") {
    Heap heap;
    auto fn = [](Heap&, const Value* args, Value* out, std::string*) {
        *out = args[0];
        return true;
    };
    ObjNative* native = heap.allocate<ObjNative>("id", 1, +fn);
    Value nv = make_obj(native);
    CHECK(is_native(nv));
    CHECK(as_native(nv)->name == "id");
    CHECK(as_native(nv)->arity == 1);
    Value arg = make_int(7);
    Value out = make_nil();
    std::string error;
    CHECK(as_native(nv)->function(heap, &arg, &out, &error));
    CHECK(as_int(out) == 7);
}

TEST_CASE("stats track allocation and collection") {
    Heap heap;
    CHECK(heap.stats().objects_allocated == 0);
    Value s = str(heap, "some string");
    HeapStats before = heap.stats();
    CHECK(before.objects_allocated == 1);
    CHECK(before.bytes_allocated >= sizeof(ObjString));
    CHECK(before.live_bytes == before.bytes_allocated);

    // Array accounting includes the element buffer, not just sizeof(ObjArray).
    heap.allocate<ObjArray>(std::size_t{100}, make_nil());
    CHECK(heap.stats().live_bytes >= before.live_bytes + sizeof(ObjArray) + 100 * sizeof(Value));

    Heap::TempRoot guard(heap, s);
    heap.collect();
    HeapStats after = heap.stats();
    CHECK(after.collections == 1);
    CHECK(after.live_objects == 1);
    CHECK(after.live_bytes == before.live_bytes);
    CHECK(after.peak_live_bytes >= before.live_bytes + 100 * sizeof(Value));
    CHECK(after.objects_allocated == 2);
}

TEST_CASE("crossing the byte threshold triggers a collection automatically") {
    Heap heap;
    for (int i = 0; i < 100; ++i) heap.allocate<ObjArray>(std::size_t{1000}, make_nil());
    CHECK(heap.stats().collections >= 1);  // 100 x 16 KB well exceeds the 1 MiB floor
    CHECK(heap.stats().live_objects < 100);
}

TEST_CASE("stress mode: everything rooted survives, garbage does not") {
    Heap heap;
    heap.set_stress(true);
    std::vector<Value> rooted;
    Heap::RootHandle handle = heap.add_root_marker([&](Heap& h) {
        for (Value v : rooted) h.mark_value(v);
    });

    for (int i = 0; i < 200; ++i) {
        // Root each new object before the next allocation, which will collect.
        Value s = make_obj(heap.intern(std::string("s") + std::to_string(i)));
        rooted.push_back(s);
        Value a = make_obj(heap.allocate<ObjArray>(std::size_t{2}, make_nil()));
        rooted.push_back(a);
        as_array(a)->elements[0] = s;
        heap.allocate<ObjArray>(std::size_t{2}, make_nil());  // immediate garbage
    }
    CHECK(heap.stats().collections >= 600);
    heap.collect();
    CHECK(heap.stats().live_objects == 400);
    for (int i = 0; i < 200; ++i) {
        CHECK(as_string(rooted[static_cast<std::size_t>(2 * i)])->chars ==
              "s" + std::to_string(i));
        CHECK(as_obj(as_array(rooted[static_cast<std::size_t>(2 * i + 1)])->elements[0]) ==
              as_obj(rooted[static_cast<std::size_t>(2 * i)]));
    }
    heap.remove_root_marker(handle);
}
