// Executable memory for the JIT: a buffer we can write ARM64 machine code into and then call.
// Compiled only on arm64 (an x86-64 CPU cannot run ARM64 code, notes D6); CMake leaves this file
// out elsewhere.
//
// Why this is not just malloc: modern operating systems refuse to let a page be writable and
// executable at the same time (W^X), because that is the easiest way for an attacker to turn a
// bug into "run my bytes". A JIT needs both, one after the other, so each OS gives us a
// sanctioned way to flip a page between the two states:
//
//   macOS arm64  Map the region once with MAP_JIT. It is then RWX on paper, but each *thread*
//                is either in "writing" or "executing" mode for such pages, switched with
//                pthread_jit_write_protect_np(0 / 1). The switch is per thread, not per page
//                and not per process: one thread can be writing new code while another keeps
//                running old code (this is what lets background compilation work, notes D8).
//   Linux arm64  Map the region read-write, write the code, then mprotect it read-execute. The
//                permission is per page and process-wide, so a rewrite means going back to
//                read-write first (and nothing may be executing that page meanwhile).
//
// No hardened-runtime entitlement is needed for an unsigned command-line binary on macOS: the
// `com.apple.security.cs.allow-jit` entitlement is only checked for processes signed with the
// hardened runtime (`codesign -o runtime`), and the linker's ad-hoc signature is not that. The
// tests in tests/unit/exec_memory_test.cpp run without any signing step, which is the evidence.
// If Rung is ever distributed hardened-runtime signed, that entitlement becomes mandatory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace rung::jit {

// Thrown when the OS refuses us memory or a permission change. Never silent: a JIT that quietly
// falls back to "no code" would hide the very failure the ladder measurements depend on.
class ExecMemoryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Owns one region of executable memory. Move-only; the destructor unmaps it, so any function
// pointer obtained from entry() dangles once the buffer is gone.
class ExecBuffer {
public:
    // Room for at least `capacity_bytes` of code. The mapping is a whole number of pages, so the
    // usable capacity (capacity_bytes()) can be a little larger.
    explicit ExecBuffer(size_t capacity_bytes);
    ~ExecBuffer();

    ExecBuffer(const ExecBuffer&) = delete;
    ExecBuffer& operator=(const ExecBuffer&) = delete;
    ExecBuffer(ExecBuffer&& other) noexcept;
    ExecBuffer& operator=(ExecBuffer&& other) noexcept;

    // Replaces the buffer's contents with `n` instruction words (each 4 bytes, ARM64 is
    // fixed-width) starting at the entry point, and leaves the memory ready to execute. Throws
    // ExecMemoryError if the words do not fit or the OS refuses a permission change.
    //
    // The steps, in this order (see the comments in the .cpp for what breaks otherwise):
    //   1. make the memory writable (for this thread)
    //   2. copy the words in
    //   3. make the memory executable again
    //   4. flush the instruction cache over the written range
    // Do not call this while another thread may be running code from this same buffer.
    void write(const uint32_t* words, size_t n);

    // The start of the code, as a function pointer of type Fn, e.g.
    //   auto f = buf.entry<int32_t (*)(int32_t, int32_t)>();
    // Call it only after write(). The caller is trusted to pick the right signature: the C++
    // compiler cannot check machine code we generated ourselves.
    template <class Fn>
    Fn entry() const {
        return reinterpret_cast<Fn>(static_cast<char*>(base_) + kHeaderBytes);
    }

    // Bytes of code that fit (the mapping minus the header).
    size_t capacity_bytes() const { return mapped_ - kHeaderBytes; }
    size_t size_bytes() const { return size_; }

private:
    // The code does not start at the first byte of the mapping: 16 zero bytes come first. Why:
    // UBSan's function check (-fsanitize=function, on in the asan preset) runs before every C++
    // call through a function pointer and *reads the 8 bytes just before the target*, looking
    // for a type signature that compiled functions carry there and our generated code does not.
    // With the code at offset 0 that read lands on the last 8 bytes of the previous page, which
    // is unmapped in roughly one run in twenty, so the test process died with a SEGV that looked
    // like a JIT bug. With a readable zero header (zeros mean "no signature", so the check
    // passes) the read is always safe. 16 rather than 8 keeps the code 16-byte aligned. Doing
    // this in the buffer protects every caller of entry().
    static constexpr size_t kHeaderBytes = 16;

    void release() noexcept;

    void* base_ = nullptr;
    size_t mapped_ = 0;  // bytes mapped, a multiple of the page size; includes the header
    size_t size_ = 0;    // bytes of code written by the last write()
};

}  // namespace rung::jit
