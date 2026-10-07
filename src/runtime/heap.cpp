#include "runtime/heap.h"

#include <algorithm>

#include "runtime/function.h"

namespace rung {

Heap::~Heap() {
    Obj* obj = objects_;
    while (obj != nullptr) {
        Obj* next = obj->next;
        free_object(obj);
        obj = next;
    }
}

void Heap::before_allocate() {
    assert(!collecting_ && "allocation during collection (root markers must not allocate)");
    if (stress_ || bytes_ > next_gc_) collect();
}

void Heap::link(Obj* obj) {
    obj->next = objects_;
    objects_ = obj;
    std::size_t size = object_bytes(*obj);
    bytes_ += size;
    stats_.bytes_allocated += size;
    stats_.objects_allocated += 1;
    stats_.live_objects += 1;
    stats_.peak_live_bytes = std::max(stats_.peak_live_bytes, bytes_);
}

ObjString* Heap::intern(std::string_view bytes) {
    auto it = strings_.find(bytes);
    if (it != strings_.end()) return it->second;
    return intern(std::string(bytes));
}

ObjString* Heap::intern(std::string&& bytes) {
    auto it = strings_.find(std::string_view(bytes));
    if (it != strings_.end()) return it->second;
    // A collection inside allocate() cannot free the string we are about to make: it does not
    // exist yet, and we looked for it above.
    ObjString* str = allocate<ObjString>(std::move(bytes));
    strings_.emplace(std::string_view(str->chars), str);
    return str;
}

Heap::RootHandle Heap::add_root_marker(RootMarker marker) {
    RootHandle handle = next_handle_++;
    root_markers_.emplace_back(handle, std::move(marker));
    return handle;
}

void Heap::remove_root_marker(RootHandle handle) {
    auto it = std::find_if(root_markers_.begin(), root_markers_.end(),
                           [handle](const auto& entry) { return entry.first == handle; });
    if (it != root_markers_.end()) root_markers_.erase(it);
}

void Heap::mark_object(Obj* obj) {
    if (obj == nullptr || obj->marked) return;  // already marked: also what stops cycles
    obj->marked = true;
    gray_.push_back(obj);  // children are visited later from the explicit stack
}

void Heap::trace(Obj* obj) {
    switch (obj->kind) {
        case ObjKind::Array:
            for (Value v : static_cast<ObjArray*>(obj)->elements) mark_value(v);
            break;
        case ObjKind::Function: {
            auto* fn = static_cast<ObjFunction*>(obj);
            mark_object(fn->name);  // null for the script
            for (Value v : fn->chunk.constants) mark_value(v);
            break;
        }
        case ObjKind::Closure: {
            auto* closure = static_cast<ObjClosure*>(obj);
            mark_object(closure->function);
            for (ObjUpvalue* upvalue : closure->upvalues) mark_object(upvalue);  // null-safe
            break;
        }
        case ObjKind::Upvalue:
            // An open upvalue's variable is on the VM stack, which the engine's root marker
            // already marks; `closed` is nil until the upvalue closes, so marking it is safe.
            mark_value(static_cast<ObjUpvalue*>(obj)->closed);
            break;
        case ObjKind::Environment: {
            auto* env = static_cast<ObjEnvironment*>(obj);
            mark_object(env->enclosing);  // null for a scope directly under the globals
            for (auto& [name, value] : env->vars) {
                mark_object(name);
                mark_value(value);
            }
            break;
        }
        case ObjKind::TreeFunction: {
            auto* fn = static_cast<ObjTreeFunction*>(obj);
            mark_object(fn->name);
            mark_object(fn->closure);
            break;
        }
        case ObjKind::String:
        case ObjKind::Native:
            break;
    }
}

void Heap::collect() {
    assert(!collecting_);
    collecting_ = true;
    for (Value v : temp_roots_) mark_value(v);
    for (auto& entry : root_markers_) entry.second(*this);
    while (!gray_.empty()) {
        Obj* obj = gray_.back();
        gray_.pop_back();
        trace(obj);
    }
    sweep();
    stats_.collections += 1;
    next_gc_ = std::max(kMinNextGc, bytes_ * 2);
    collecting_ = false;
}

void Heap::sweep() {
    Obj** link = &objects_;
    std::size_t live = 0;
    while (*link != nullptr) {
        Obj* obj = *link;
        if (obj->marked) {
            obj->marked = false;
            live += object_bytes(*obj);
            link = &obj->next;
        } else {
            *link = obj->next;
            free_object(obj);
            stats_.live_objects -= 1;
        }
    }
    bytes_ = live;
}

void Heap::free_object(Obj* obj) {
    switch (obj->kind) {
        case ObjKind::String: {
            auto* str = static_cast<ObjString*>(obj);
            strings_.erase(std::string_view(str->chars));  // weak table: erase before freeing
            delete str;
            break;
        }
        case ObjKind::Array:
            delete static_cast<ObjArray*>(obj);
            break;
        case ObjKind::Native:
            delete static_cast<ObjNative*>(obj);
            break;
        case ObjKind::Function:
            delete static_cast<ObjFunction*>(obj);
            break;
        case ObjKind::Closure:
            delete static_cast<ObjClosure*>(obj);
            break;
        case ObjKind::Upvalue:
            delete static_cast<ObjUpvalue*>(obj);
            break;
        case ObjKind::Environment:
            delete static_cast<ObjEnvironment*>(obj);
            break;
        case ObjKind::TreeFunction:
            delete static_cast<ObjTreeFunction*>(obj);
            break;
    }
}

HeapStats Heap::stats() const {
    HeapStats s = stats_;
    s.live_bytes = bytes_;
    return s;
}

}  // namespace rung
