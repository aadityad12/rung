#include "jit/jit.h"

#if !RUNG_JIT
#error "jit.cpp needs RUNG_JIT (arm64 and RUNG_NANBOX); CMake must not build it elsewhere"
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <utility>

#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

#include "disassembler.h"
#include "jit/arm64_emitter.h"
#include "jit/jit_compiler.h"
#include "runtime/heap.h"

namespace rung::jit {

namespace {

std::string function_name(const ObjFunction& function) {
    return function.name != nullptr ? function.name->chars : "<script>";
}

// The QoS class of the calling thread, so a job runs at the priority of the thread that asked
// for it (see worker_main). macOS only; 0 ("unspecified") elsewhere.
unsigned current_qos() {
#ifdef __APPLE__
    return static_cast<unsigned>(qos_class_self());
#else
    return 0;
#endif
}

}  // namespace

Jit::Jit(std::uint32_t threshold, std::FILE* log, bool background)
    : threshold_(threshold), log_(log), background_(background) {
    if (background_) worker_ = std::thread([this] { worker_main(); });
}

Jit::~Jit() { shutdown(); }

// ---- compiling one function ------------------------------------------------------------------

// Runs on either thread. Everything it reads from `function` is fixed before the program starts
// running: the register compiler builds every function's code, constants and name up front, and
// nothing changes them afterwards (the VM's run-time state, such as the inline cache and the
// hotness count, lives in other fields). Everything it writes is its own: the generated words,
// a fresh mapping no other thread knows about yet, and the returned struct.
Jit::Built Jit::build(const ObjFunction& function) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    const RegChunk& chunk = function.reg;

    // Why the function stays in the VM, or empty if it compiles.
    std::string rejected_because;
    GeneratedCode code;
    if (std::optional<Rejection> rejection = check_whitelist(chunk)) {
        rejected_because = "instruction " + std::to_string(rejection->index) + " (" +
                           reg_opcode_name(unfused_op(chunk.code[rejection->index])) + ") " +
                           rejection->reason;
    } else {
        try {
            code = generate(chunk);
        } catch (const arm64::EmitError& error) {
            rejected_because = error.what();
        }
    }

    Built built;
    if (rejected_because.empty()) {
        built.bytes = code.words.size() * sizeof(std::uint32_t);
        try {
            // On macOS, write() switches *this thread* to writing MAP_JIT pages and back
            // (pthread_jit_write_protect_np is per thread), so on the compiler thread the
            // engine's thread keeps running other compiled code meanwhile (notes D8).
            ExecBuffer buffer(built.bytes);
            buffer.write(code.words.data(), code.words.size());
            built.entry = buffer.entry<JitEntry>();
            built.buffer = std::move(buffer);
        } catch (const ExecMemoryError& error) {
            // The OS refused executable memory. Falling back to the VM quietly would hide the
            // very failure the JIT measurements depend on, so stop loudly (exec_memory.h).
            std::fprintf(stderr, "rung: jit: %s\n", error.what());
            std::abort();
        }
    }
    built.ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());

    if (built.buffer) {
        built.log_line = "[jit] compiled " + function_name(function) + ": " +
                         std::to_string(chunk.code.size()) + " bytecode instructions -> " +
                         std::to_string(code.words.size()) + " machine instructions (" +
                         std::to_string(built.bytes) + " bytes) in " + std::to_string(built.ns) +
                         " ns";
    } else {
        built.log_line = "[jit] rejected " + function_name(function) + ": " + rejected_because;
    }
    return built;
}

void Jit::record_locked(Built& built) {
    compile_ns_ += built.ns;
    if (built.buffer) {
        buffers_.push_back(std::move(*built.buffer));
        built.buffer.reset();
        code_bytes_ += built.bytes;
        ++compiled_;
    } else {
        ++rejected_;
    }
}

void Jit::compile(ObjFunction& function) {
    Built built = build(function);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        record_locked(built);
    }
    if (built.entry != nullptr) {
        // Same thread as the caller, so the ordering is trivially right; the store is atomic
        // only because the field is (notes D8).
        function.jit_entry.store(built.entry, std::memory_order_release);
        function.jit_status = JitStatus::Compiled;
    } else {
        function.jit_status = JitStatus::Rejected;
    }
    if (log_ != nullptr) std::fprintf(log_, "%s\n", built.log_line.c_str());
}

// ---- background compilation (notes D8) --------------------------------------------------------

void Jit::enqueue(ObjFunction& function) {
    function.jit_status = JitStatus::Queued;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        queue_.push_back(Job{&function, current_qos()});
    }
    work_ready_.notify_one();
}

void Jit::adopt(ObjFunction& function, JitEntry entry) {
    // Read the code's first instruction word as data, in C++, once. ThreadSanitizer cannot see
    // instruction fetches, so without this read nothing on this thread would ever touch the
    // bytes the compiler thread wrote, and a broken handoff (a relaxed store, say) would go
    // unreported. With it, TSan checks that the compiler's stores of the code (plain stores,
    // see ExecBuffer::write) happen-before this thread runs it. A plain load, not memcpy, for
    // the same reason as there. It is also a cheap sanity check: generated code never starts
    // with 0, which is `udf #0`, a guaranteed crash.
    const std::uint32_t first_word =
        *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uintptr_t>(entry));
    if (first_word == 0) {
        std::fprintf(stderr, "rung: jit: published code for %s is empty\n",
                     function_name(function).c_str());
        std::abort();
    }
    // The compiler thread cleaned the data cache and invalidated the instruction cache for the
    // new code before publishing it (ExecBuffer::write), and those invalidations reach every
    // core. What they cannot reach is this core's pipeline: the ARM architecture requires the
    // core that will execute code another core wrote to perform a context synchronisation
    // event (ISB) after it has seen the code and before it runs it, so no instruction fetched
    // earlier survives. Once per function is enough: after this, the code's addresses can only
    // ever be fetched fresh.
    __asm__ volatile("isb" ::: "memory");
    function.jit_status = JitStatus::Compiled;
}

void Jit::worker_main() {
    unsigned qos = current_qos();
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        work_ready_.wait(lock, [this] { return stopping_ || (!held_ && !queue_.empty()); });
        if (stopping_) return;  // shutdown() already emptied the queue
        Job job = queue_.front();
        queue_.pop_front();
        // From here until compiling_ is cleared below, mark_pending keeps the function alive.
        compiling_ = job.function;
        lock.unlock();

#ifdef __APPLE__
        // Run at the QoS of the thread that queued the job. In bench mode that thread is
        // QOS_CLASS_USER_INTERACTIVE (notes D5), so macOS prefers a performance core for the
        // compiler too, and the measurement compares compiling on another performance core with
        // compiling inline, not with compiling on an efficiency core.
        if (job.qos != qos && job.qos != static_cast<unsigned>(QOS_CLASS_UNSPECIFIED)) {
            pthread_set_qos_class_self_np(static_cast<qos_class_t>(job.qos), 0);
            qos = job.qos;
        }
#endif
        Built built = build(*job.function);

        // The handoff. Release: every write this thread made before it (the machine code, the
        // instruction cache maintenance) happens-before anything a thread does after an acquire
        // load that sees this pointer. With a plain or relaxed store, the compiler or the CPU
        // may make the pointer visible before the code bytes, and the engine could jump into a
        // half-written buffer. The function is still in compiling_, so the collector cannot
        // have freed it. The store is deliberately made before taking the lock: the lock would
        // also order the two threads whenever the engine's thread takes it next (to queue a
        // function or to collect), which would hide a broken store from ThreadSanitizer; this
        // way the atomic is the only thing ordering the code bytes before the engine reads them.
        if (built.entry != nullptr) {
            job.function->jit_entry.store(built.entry, std::memory_order_release);
        }

        lock.lock();
        record_locked(built);  // the ExecBuffer moves; the mapping, and so the entry, does not
        compiling_ = nullptr;  // the compiler is done with the function: it is no longer a root
        if (queue_.empty()) idle_.notify_all();

        if (log_ != nullptr) {
            lock.unlock();
            std::fprintf(log_, "%s on the compiler thread\n", built.log_line.c_str());
            lock.lock();
        }
    }
}

void Jit::mark_pending(Heap& heap) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Job& job : queue_) heap.mark_object(job.function);
    if (compiling_ != nullptr) heap.mark_object(compiling_);
}

void Jit::wait_until_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return (queue_.empty() || stopping_) && compiling_ == nullptr; });
}

void Jit::hold_for_testing(bool hold) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        held_ = hold;
    }
    work_ready_.notify_all();
}

void Jit::shutdown() {
    if (!worker_.joinable()) return;
    std::vector<ObjFunction*> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        for (const Job& job : queue_) dropped.push_back(job.function);
        queue_.clear();
    }
    work_ready_.notify_all();
    worker_.join();
    idle_.notify_all();
    // Logged on this thread after the join. The functions are still alive: the engine calls
    // shutdown before it stops marking its roots, and only this thread ever collects.
    if (log_ != nullptr) {
        for (const ObjFunction* function : dropped) {
            std::fprintf(log_, "[jit] cancelled %s: still queued at exit\n",
                         function_name(*function).c_str());
        }
    }
}

// ---- reporting -------------------------------------------------------------------------------

void Jit::note_bailout(const ObjFunction& function, std::size_t index) {
    ++bailouts_;
    if (log_ != nullptr) {
        const RegChunk& chunk = function.reg;
        std::fprintf(log_, "[jit] bail-out in %s at instruction %zu (%s, line %d)\n",
                     function_name(function).c_str(), index,
                     reg_opcode_name(insn_op(chunk.code[index])), chunk.line_at(index));
    }
}

std::string Jit::stats_report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text = "jit: " + std::to_string(compiled_) + " functions compiled (" +
                       std::to_string(code_bytes_) + " bytes of code), " +
                       std::to_string(rejected_) + " rejected, " + std::to_string(bailouts_) +
                       " bail-outs, " + std::to_string(compile_ns_) + " ns compiling";
    if (background_) {
        const std::size_t pending = queue_.size() + (compiling_ != nullptr ? 1 : 0);
        text += " on the compiler thread, " + std::to_string(pending) +
                " queued or in progress";
    }
    return text + "\n";
}

std::uint64_t Jit::total_compile_ns() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return compile_ns_;
}

std::uint64_t Jit::functions_compiled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return compiled_;
}

std::uint64_t Jit::functions_rejected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rejected_;
}

}  // namespace rung::jit
