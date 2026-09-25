#include "output.h"

namespace rung {

void Output::flush() {
    if (buffer_.empty()) return;  // whatever was written before was already flushed with it
    std::fwrite(buffer_.data(), 1, buffer_.size(), file_);
    buffer_.clear();
    std::fflush(file_);
}

}  // namespace rung
