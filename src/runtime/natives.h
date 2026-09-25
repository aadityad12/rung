#pragma once

#include <functional>
#include <string_view>

#include "runtime/heap.h"
#include "runtime/object.h"

namespace rung {

// The three built-in functions (notes D2, §2.5): clock(), len(x), array(n, fill). Each has the
// NativeFn signature. The caller checks arity first (using arity_error_message from ops.h), so
// `args` always holds exactly `arity` values.
bool native_clock(Heap& heap, const Value* args, Value* out, std::string* error);
bool native_len(Heap& heap, const Value* args, Value* out, std::string* error);
bool native_array(Heap& heap, const Value* args, Value* out, std::string* error);

// Creates all three natives and hands each to `define`, so engines don't each list them. The
// engine's callback stores the native in its global table under `name`.
//
// GC contract: creating a native allocates and may collect, so each native is rooted only while
// `define` runs. `define` must store it somewhere the engine's root marker reaches (e.g. the
// globals table) before returning, and that table must already be registered with the heap.
void register_natives(Heap& heap,
                      const std::function<void(std::string_view name, Value native)>& define);

}  // namespace rung
