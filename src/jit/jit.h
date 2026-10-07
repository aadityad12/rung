// The baseline JIT (Engine 4, notes D16): owns the machine code of every function it compiled,
// and keeps the numbers --jit-log and --stats report. The register VM decides *when* to compile
// (hotness) and runs the code; this class decides *whether* a function can be compiled and
// does it, either at once on the engine's thread or, with --jit-background (Engine 5, notes
// D8), on a compiler thread of its own.
//
// Built only where the JIT exists: an arm64 CPU (the code is ARM64) and the NaN-boxed value
// layout (its type guards test NaN-box tags, notes D3). CMake defines RUNG_JIT there.
//
// The background protocol (notes D8, "Handoff and GC protocol"), in short:
//   - The engine's thread queues a hot function (enqueue) and carries on in the VM.
//   - The compiler thread reads only what never changes after compilation: the function's
//     register bytecode, its constants and its name. It writes nothing on the heap except the
//     function's atomic jit_entry, and allocates nothing on the Heap (which is not thread-safe).
//   - It publishes finished code with a release store to jit_entry; the engine's thread reads
//     jit_entry with an acquire load, in C++, before it calls machine code. The machine code
//     itself touches only the register file, which the compiler thread never touches, so
//     everything the two threads share is ordinary C++ that ThreadSanitizer can check.
//   - A queued or in-progress function is a GC root (mark_pending) until the compiler thread
//     has published it, so the collector cannot free what the compiler is reading.
//   - Shutting down drops what is still queued, lets the compile in progress finish, and joins.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "jit/exec_memory.h"
#include "runtime/function.h"

namespace rung {
class Heap;
}

namespace rung::jit {

class Jit {
public:
    // `log` is where --jit-log writes (stderr from the CLI), or null for no log. `background`
    // starts the compiler thread (--jit-background).
    Jit(std::uint32_t threshold, std::FILE* log, bool background = false);
    // Shuts the compiler thread down (see shutdown) before the machine code is unmapped.
    ~Jit();
    Jit(const Jit&) = delete;
    Jit& operator=(const Jit&) = delete;

    // Calls plus loop back-edges after which a function is compiled (--jit-threshold).
    std::uint32_t threshold() const { return threshold_; }
    bool background() const { return background_; }

    // Synchronous mode. Compiles `function`'s register code on the calling thread. On success
    // sets jit_entry and status Compiled; if the function is outside the whitelist, sets status
    // Rejected and the VM keeps running it. Either way it is never tried again.
    void compile(ObjFunction& function);

    // Background mode, engine's thread. Queues `function` for the compiler thread and sets its
    // status to Queued. From here until the compiler thread publishes it, the function is a GC
    // root through mark_pending. If it compiles, jit_entry becomes non-null at some later time;
    // if it is rejected, jit_entry stays null and the function stays Queued (and in the VM).
    void enqueue(ObjFunction& function);

    // Engine's thread, the first time it finds a non-null jit_entry (`entry`) on a Queued
    // function: the code was written by another core, so this core must discard any
    // instructions it may have fetched early before it runs them (an ISB). Also reads the
    // code's first word in C++, which is what lets ThreadSanitizer check the handoff (see the
    // .cpp). Sets status Compiled, so later calls skip this.
    static void adopt(ObjFunction& function, JitEntry entry);

    // Root marker for the collector (notes D10): marks every function that is queued or being
    // compiled. Called by the engine's root marker, on the engine's thread, during collection.
    void mark_pending(Heap& heap);

    // Blocks until nothing is queued or being compiled. For tests and tools that want a
    // deterministic point; the engine itself never waits for the compiler.
    void wait_until_idle();

    // While held, the compiler thread takes no new job (one already started still finishes).
    // For tests: lets a test observe a function while it is queued.
    void hold_for_testing(bool hold);

    // Stops the compiler thread and joins it: what is still queued is dropped (logged as
    // cancelled), the compile in progress finishes and is recorded. Idempotent. The engine calls
    // it before it unregisters its GC roots, so a function being compiled is still rooted while
    // the compiler reads it.
    void shutdown();

    // The VM reports every exit from machine code that was not a return: a failed type guard
    // or a zero divisor in the instruction at `index`. Engine's thread only.
    void note_bailout(const ObjFunction& function, std::size_t index);

    // One line for --stats.
    std::string stats_report() const;
    // Wall-clock time spent compiling, every function together (whitelist check, code
    // generation, mapping and writing executable memory), on whichever thread compiled it.
    std::uint64_t total_compile_ns() const;
    std::uint64_t functions_compiled() const;
    std::uint64_t functions_rejected() const;
    std::uint64_t bailouts() const { return bailouts_; }

private:
    // What compiling one function produced. Built without touching anything shared.
    struct Built {
        std::optional<ExecBuffer> buffer;  // the code; empty if the function was rejected
        JitEntry entry = nullptr;
        std::size_t bytes = 0;
        std::uint64_t ns = 0;
        std::string log_line;  // what --jit-log prints for it
    };
    // A queued function, with the QoS class of the thread that queued it (macOS; 0 elsewhere).
    struct Job {
        ObjFunction* function;
        unsigned qos;
    };

    // Whitelist check, code generation and executable memory for one function. Reads only the
    // function's immutable parts (its register code, constants and name); writes nothing shared.
    static Built build(const ObjFunction& function);
    // Keeps the code and counts it. Caller holds mutex_.
    void record_locked(Built& built);
    void worker_main();

    const std::uint32_t threshold_;
    std::FILE* const log_;
    const bool background_;

    // Everything below up to worker_ is guarded by mutex_. In synchronous mode there is no
    // other thread, and the lock is simply uncontended.
    mutable std::mutex mutex_;
    std::condition_variable work_ready_;  // a job was queued, or shutdown/hold changed
    std::condition_variable idle_;        // the queue emptied and nothing is in progress
    std::deque<Job> queue_;
    ObjFunction* compiling_ = nullptr;    // the function the compiler thread is reading
    bool stopping_ = false;
    bool held_ = false;
    // One mapping per compiled function. Never freed before the JIT is, even if the function
    // object is collected, so a jit_entry can never dangle while the engine runs.
    std::vector<ExecBuffer> buffers_;
    std::uint64_t compiled_ = 0;
    std::uint64_t rejected_ = 0;
    std::uint64_t code_bytes_ = 0;
    std::uint64_t compile_ns_ = 0;
    std::thread worker_;

    // Engine's thread only: the compiler thread never sees a bail-out.
    std::uint64_t bailouts_ = 0;
};

}  // namespace rung::jit
