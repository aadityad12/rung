#include "engine.h"

#include "engine_tree.h"
#include "vm_reg.h"
#include "vm_stack.h"

namespace rung {

std::unique_ptr<Engine> make_engine(std::string_view name, Heap& heap, Output& out,
                                    const EngineOptions& options) {
    if (name == "register") {
        return std::make_unique<RegisterEngine>(heap, out, options.inline_cache,
                                                options.superinstructions);
    }
#if RUNG_JIT
    // The register VM with the JIT attached, so it takes the register VM's rungs too (the ladder
    // is cumulative, notes D4).
    if (name == "jit") return std::make_unique<RegisterEngine>(heap, out, options);
#endif
    // The other engines have neither rung. A request for one is refused rather than ignored,
    // so a benchmark can never label a run with a rung it did not use.
    if (options.inline_cache || options.superinstructions) return nullptr;
    if (name == "tree") return std::make_unique<TreeEngine>(heap, out);
    if (name == "stack") return std::make_unique<StackEngine>(heap, out);
    return nullptr;
}

std::vector<std::string_view> engine_names() {
    std::vector<std::string_view> names = {"tree", "stack", "register"};
    if (jit_available()) names.push_back("jit");
    return names;
}

}  // namespace rung
