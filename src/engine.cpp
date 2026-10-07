#include "engine.h"

#include "engine_tree.h"
#include "vm_reg.h"
#include "vm_stack.h"

namespace rung {

std::unique_ptr<Engine> make_engine(std::string_view name, Heap& heap, Output& out,
                                    EngineOptions options) {
    if (name == "register") {
        return std::make_unique<RegisterEngine>(heap, out, options.inline_cache);
    }
    // The other engines have no inline cache. A request for one is refused rather than ignored,
    // so a benchmark can never label a run with a rung it did not use.
    if (options.inline_cache) return nullptr;
    if (name == "tree") return std::make_unique<TreeEngine>(heap, out);
    if (name == "stack") return std::make_unique<StackEngine>(heap, out);
    return nullptr;
}

std::vector<std::string_view> engine_names() { return {"tree", "stack", "register"}; }

}  // namespace rung
