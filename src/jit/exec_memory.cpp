#include "jit/exec_memory.h"

#if !defined(__aarch64__)
#error "exec_memory.cpp is arm64-only; CMake must not build it on other architectures"
#endif

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace rung::jit {

namespace {

[[noreturn]] void fail(const char* what) {
    throw ExecMemoryError(std::string(what) + ": " + std::strerror(errno));
}

size_t round_up_to_page(size_t bytes) {
    size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return (bytes + page - 1) / page * page;  // bytes >= kHeaderBytes > 0 at the call site
}

}  // namespace

ExecBuffer::ExecBuffer(size_t capacity_bytes)
    : mapped_(round_up_to_page(kHeaderBytes + capacity_bytes)) {
#if defined(__APPLE__)
    // MAP_JIT is what allows a RWX mapping at all on Apple silicon; without it, asking for
    // PROT_WRITE|PROT_EXEC fails outright. The mapping starts in this thread's "executing" mode.
    void* p = mmap(nullptr, mapped_, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#else
    // Read-write only. write() adds execute permission and removes write permission (W^X).
    void* p = mmap(nullptr, mapped_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (p == MAP_FAILED) {
        mapped_ = 0;
        fail("mmap for executable memory failed");
    }
    base_ = p;
}

ExecBuffer::~ExecBuffer() { release(); }

ExecBuffer::ExecBuffer(ExecBuffer&& other) noexcept
    : base_(std::exchange(other.base_, nullptr)),
      mapped_(std::exchange(other.mapped_, 0)),
      size_(std::exchange(other.size_, 0)) {}

ExecBuffer& ExecBuffer::operator=(ExecBuffer&& other) noexcept {
    if (this != &other) {
        release();
        base_ = std::exchange(other.base_, nullptr);
        mapped_ = std::exchange(other.mapped_, 0);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void ExecBuffer::release() noexcept {
    if (base_ != nullptr) munmap(base_, mapped_);
    base_ = nullptr;
    mapped_ = 0;
    size_ = 0;
}

void ExecBuffer::write(const uint32_t* words, size_t n) {
    if (base_ == nullptr) throw ExecMemoryError("write to a moved-from ExecBuffer");
    if (n > capacity_bytes() / sizeof(uint32_t)) {
        throw ExecMemoryError("code does not fit in ExecBuffer");
    }
    size_t bytes = n * sizeof(uint32_t);
    char* code = static_cast<char*>(base_) + kHeaderBytes;

    // Step 1: make the memory writable.
#if defined(__APPLE__)
    // Per-thread switch: 0 means "this thread may write MAP_JIT pages (and may not execute
    // them)". Other threads are unaffected, which is why the background compiler (notes D8) can
    // write while the main thread runs. If we forget this, the memcpy below faults with
    // EXC_BAD_ACCESS the first time we touch the page.
    pthread_jit_write_protect_np(0);
#else
    // Needed on a second write(): the previous call left the pages read-execute.
    if (mprotect(base_, mapped_, PROT_READ | PROT_WRITE) != 0) {
        fail("mprotect to read-write failed");
    }
#endif

    // Step 2: copy the instruction words in, after the zero header (see kHeaderBytes). memcpy
    // says plainly that this is raw bytes going into memory the type system knows nothing about.
    if (bytes != 0) std::memcpy(code, words, bytes);
    size_ = bytes;

    // Step 3: make the memory executable again.
#if defined(__APPLE__)
    // Back to "executing" mode on this thread. Leaving it in writing mode would make the very
    // next call into the code crash, because a thread in writing mode cannot execute JIT pages.
    pthread_jit_write_protect_np(1);
#else
    if (mprotect(base_, mapped_, PROT_READ | PROT_EXEC) != 0) {
        fail("mprotect to read-execute failed");
    }
#endif

    // Step 4: flush the instruction cache. ARM64 has separate instruction and data caches that
    // the hardware does not keep coherent: our memcpy went through the data cache, but the CPU
    // fetches instructions through the instruction cache, which may still hold the old bytes
    // (or garbage) for these addresses. The builtin emits the required clean/invalidate
    // sequence (`dc cvau`, `ic ivau`, barriers; sys_icache_invalidate on macOS).
    //
    // What goes wrong with the wrong order: flushing before the copy invalidates nothing useful,
    // and the CPU can run stale instructions. It is nasty because it is intermittent: a fresh
    // buffer that was never executed usually works, and a debug build, which is slower between
    // write and call, often hides it; a release build, or a buffer being overwritten (the old
    // code is likely cached), crashes or silently runs the previous code. Likewise, calling
    // before step 3 faults (macOS) or hits a non-executable page (Linux).
    __builtin___clear_cache(code, code + bytes);
}

}  // namespace rung::jit
