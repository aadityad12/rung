// ExecBuffer tests: actually run machine code we write at runtime. arm64 only; the file is not
// compiled elsewhere. The instruction words are checked against LLVM in arm64_emitter_test.cpp;
// here they are literals so this test does not depend on the emitter.
#include <doctest.h>

#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "jit/exec_memory.h"

using rung::jit::ExecBuffer;
using rung::jit::ExecMemoryError;

namespace {

constexpr uint32_t kMovW0Imm42 = 0x52800540;  // mov w0, #42
constexpr uint32_t kMovW0Imm7 = 0x528000E0;   // mov w0, #7
constexpr uint32_t kAddW0W0W1 = 0x0B010000;   // add w0, w0, w1
constexpr uint32_t kSubW0W0W1 = 0x4B010000;   // sub w0, w0, w1
constexpr uint32_t kRet = 0xD65F03C0;         // ret

using Nullary = int32_t (*)();
using Binary = int32_t (*)(int32_t, int32_t);

}  // namespace

TEST_CASE("exec memory: run a function that returns 42") {
    ExecBuffer buf(4096);
    const uint32_t code[] = {kMovW0Imm42, kRet};
    buf.write(code, 2);
    CHECK(buf.size_bytes() == 8);
    CHECK(buf.entry<Nullary>()() == 42);
}

TEST_CASE("exec memory: run a function that adds its two arguments") {
    ExecBuffer buf(4096);
    const uint32_t code[] = {kAddW0W0W1, kRet};
    buf.write(code, 2);
    auto add = buf.entry<Binary>();
    CHECK(add(2, 3) == 5);
    CHECK(add(-10, 4) == -6);
    CHECK(add(INT32_MAX, 1) == INT32_MIN);  // 32-bit wraparound, notes D1
}

TEST_CASE("exec memory: overwriting a buffer runs the new code") {
    // If the instruction cache were not flushed, the CPU could keep running the first program
    // after the second was written. Repeat to make a stale-cache bug likely to show.
    ExecBuffer buf(4096);
    for (int i = 0; i < 200; ++i) {
        const uint32_t first[] = {kMovW0Imm42, kRet};
        buf.write(first, 2);
        CHECK(buf.entry<Nullary>()() == 42);

        const uint32_t second[] = {kMovW0Imm7, kRet};
        buf.write(second, 2);
        CHECK(buf.entry<Nullary>()() == 7);
    }

    // A different function over the same bytes.
    const uint32_t third[] = {kSubW0W0W1, kRet};
    buf.write(third, 2);
    CHECK(buf.entry<Binary>()(10, 3) == 7);
}

TEST_CASE("exec memory: capacity covers the request and overflow is an error") {
    ExecBuffer buf(1);
    CHECK(buf.capacity_bytes() >= 1);
    CHECK(buf.capacity_bytes() % 4 == 0);  // a whole number of instruction words

    std::vector<uint32_t> too_big(buf.capacity_bytes() / 4 + 1, kRet);
    CHECK_THROWS_AS(buf.write(too_big.data(), too_big.size()), ExecMemoryError);

    // Exactly full is fine.
    std::vector<uint32_t> full(buf.capacity_bytes() / 4, kRet);
    full[0] = kMovW0Imm42;
    buf.write(full.data(), full.size());
    CHECK(buf.entry<Nullary>()() == 42);
}

TEST_CASE("exec memory: a moved buffer keeps working and the source is inert") {
    ExecBuffer a(4096);
    const uint32_t code[] = {kMovW0Imm42, kRet};
    a.write(code, 2);

    ExecBuffer b(std::move(a));
    CHECK(b.entry<Nullary>()() == 42);
    CHECK_THROWS_AS(a.write(code, 2), ExecMemoryError);  // moved-from on purpose

    ExecBuffer c(4096);
    c = std::move(b);
    CHECK(c.entry<Nullary>()() == 42);
}

TEST_CASE("exec memory: one thread writes while another runs finished code (notes D8)") {
    // On macOS the write-protect switch is per thread. The main thread keeps executing `runner`
    // while a second thread writes and then executes its own buffer. Each thread only touches
    // its own buffer, so this is race-free and ThreadSanitizer-clean by construction.
    ExecBuffer runner(4096);
    const uint32_t code[] = {kMovW0Imm42, kRet};
    runner.write(code, 2);

    ExecBuffer worker_buf(4096);
    int32_t worker_result = 0;
    std::thread worker([&] {
        const uint32_t add[] = {kAddW0W0W1, kRet};
        for (int i = 0; i < 100; ++i) worker_buf.write(add, 2);
        worker_result = worker_buf.entry<Binary>()(20, 22);
    });

    int32_t sum = 0;
    for (int i = 0; i < 1000; ++i) sum += runner.entry<Nullary>()();
    worker.join();

    CHECK(sum == 42000);
    CHECK(worker_result == 42);
}

TEST_CASE("exec memory: code written on one thread runs on another after a handoff") {
    // The background compiler's real pattern: compile on a helper thread, run on the main one.
    ExecBuffer buf(4096);
    std::thread writer([&] {
        const uint32_t add[] = {kAddW0W0W1, kRet};
        buf.write(add, 2);
    });
    writer.join();  // the join is the happens-before edge, as the atomic handoff will be
    CHECK(buf.entry<Binary>()(40, 2) == 42);
}

TEST_CASE("exec memory: the bytes before the entry point are readable") {
    // UBSan's function check reads 8 bytes before every called function pointer. The entry point
    // must never sit at the very start of a mapping, or that read can hit an unmapped page and
    // crash about one run in twenty under the asan preset (see kHeaderBytes in exec_memory.h).
    // Many buffers, so a placement that happens to be safe is not mistaken for a fix.
    for (int i = 0; i < 300; ++i) {
        ExecBuffer buf(4096);
        const uint32_t code[] = {kMovW0Imm42, kRet};
        buf.write(code, 2);
        auto entry = reinterpret_cast<const unsigned char*>(buf.entry<Nullary>());
        volatile unsigned char sink = 0;
        for (int back = 1; back <= 8; ++back) sink = entry[-back];  // must not fault
        (void)sink;
        CHECK(buf.entry<Nullary>()() == 42);
    }
}
