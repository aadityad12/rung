#include "engine.h"

#include "engine_tree.h"

namespace rung {

std::unique_ptr<Engine> make_engine(std::string_view name, Heap& heap, Output& out) {
    if (name == "tree") return std::make_unique<TreeEngine>(heap, out);
    return nullptr;
}

std::vector<std::string_view> engine_names() { return {"tree"}; }

}  // namespace rung
