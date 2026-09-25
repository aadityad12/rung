#pragma once

#include <cassert>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/object.h"
#include "runtime/value.h"

namespace rung {

struct HeapStats {
    std::size_t objects_allocated = 0;  // total ever allocated
    std::size_t bytes_allocated = 0;    // total ever allocated (cumulative)
    std::size_t collections = 0;
    std::size_t peak_live_bytes = 0;    // high-water mark of bytes held, garbage included
    std::size_t live_bytes = 0;         // bytes held right now
    std::size_t live_objects = 0;       // objects held right now
};

// Owns every heap object and runs a precise, stop-the-world mark-sweep collector (notes D10).
//
// NOT thread-safe: allocation, collection and root registration must all happen on one thread
// at a time. The background-compilation work must never touch a Heap from its compiler thread.
class Heap {
public:
    using RootMarker = std::function<void(Heap&)>;
    using RootHandle = std::size_t;

    // Protects a Value that lives only in a C++ local while code that may allocate runs.
    // Strictly LIFO.
    class TempRoot {
    public:
        TempRoot(Heap& heap, Value value) : heap_(heap) {
            heap_.temp_roots_.push_back(value);
            depth_ = heap_.temp_roots_.size();
        }
        ~TempRoot() {
            assert(heap_.temp_roots_.size() == depth_ && "TempRoot destroyed out of LIFO order");
            heap_.temp_roots_.pop_back();
        }
        TempRoot(const TempRoot&) = delete;
        TempRoot& operator=(const TempRoot&) = delete;

    private:
        Heap& heap_;
        std::size_t depth_;
    };

    Heap() = default;
    ~Heap();
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    // May collect first, so any Value the caller holds only in a local (including ones passed
    // in `args`, such as an array's element vector) must be rooted by the caller.
    template <class T, class... Args>
    T* allocate(Args&&... args) {
        before_allocate();
        T* obj = new T(std::forward<Args>(args)...);
        link(obj);
        return obj;
    }

    // Returns the one string with these bytes, creating it if needed. Weak: an interned string
    // that nothing else reaches is freed by the next collection.
    ObjString* intern(std::string_view bytes);
    ObjString* intern(std::string&& bytes);

    RootHandle add_root_marker(RootMarker marker);
    void remove_root_marker(RootHandle handle);

    // For root markers (and tracing) to call.
    void mark_value(Value v) {
        if (is_obj(v)) mark_object(as_obj(v));
    }
    void mark_object(Obj* obj);

    void collect();
    void set_stress(bool stress) { stress_ = stress; }
    HeapStats stats() const;

private:
    struct FnvHash {
        std::size_t operator()(std::string_view s) const { return fnv1a(s); }
    };

    static constexpr std::size_t kMinNextGc = 1024 * 1024;

    void before_allocate();
    void link(Obj* obj);
    void trace(Obj* obj);
    void sweep();
    void free_object(Obj* obj);

    Obj* objects_ = nullptr;
    std::vector<Obj*> gray_;
    std::vector<Value> temp_roots_;
    std::vector<std::pair<RootHandle, RootMarker>> root_markers_;
    RootHandle next_handle_ = 1;
    // Keys view into the strings' own buffers; the objects are heap-allocated so those stay
    // put, and sweep erases the entry before freeing the string.
    std::unordered_map<std::string_view, ObjString*, FnvHash> strings_;

    bool stress_ = false;
    bool collecting_ = false;
    std::size_t bytes_ = 0;
    std::size_t next_gc_ = kMinNextGc;
    HeapStats stats_;
};

}  // namespace rung
