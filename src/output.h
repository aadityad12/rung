#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace rung {

// The one place a Rung program's stdout goes through (notes D12). `print` appends to a buffer
// that is written out when it grows large, when flush() is called, and on destruction. main
// calls flush() before anything is written to stderr, so the two streams appear in the order the
// events happened even when both are the same terminal or file.
class Output {
public:
    explicit Output(std::FILE* file = stdout) : file_(file) {}
    ~Output() { flush(); }
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    void write(std::string_view text) {
        buffer_.append(text);
        if (buffer_.size() >= kFlushThreshold) flush();
    }
    void flush();

private:
    static constexpr std::size_t kFlushThreshold = 64 * 1024;

    std::FILE* file_;
    std::string buffer_;
};

}  // namespace rung
