#include "jit/jit.h"

#if !RUNG_JIT
#error "jit.cpp needs RUNG_JIT (arm64 and RUNG_NANBOX); CMake must not build it elsewhere"
#endif

#include <chrono>
#include <cstdlib>
#include <optional>
#include <utility>

#include "disassembler.h"
#include "jit/arm64_emitter.h"
#include "jit/jit_compiler.h"

namespace rung::jit {

namespace {

std::string function_name(const ObjFunction& function) {
    return function.name != nullptr ? function.name->chars : "<script>";
}

}  // namespace

Jit::Jit(std::uint32_t threshold, std::FILE* log) : threshold_(threshold), log_(log) {}

void Jit::compile(ObjFunction& function) {
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
    if (!rejected_because.empty()) {
        function.jit_status = JitStatus::Rejected;
        compile_ns_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        ++rejected_;
        if (log_ != nullptr) {
            std::fprintf(log_, "[jit] rejected %s: %s\n", function_name(function).c_str(),
                         rejected_because.c_str());
        }
        return;
    }

    const std::size_t bytes = code.words.size() * sizeof(std::uint32_t);
    try {
        ExecBuffer buffer(bytes);
        buffer.write(code.words.data(), code.words.size());
        function.jit_entry = buffer.entry<JitEntry>();
        buffers_.push_back(std::move(buffer));
    } catch (const ExecMemoryError& error) {
        // The OS refused executable memory. Falling back to the VM quietly would hide the very
        // failure the JIT measurements depend on, so stop loudly (exec_memory.h).
        std::fprintf(stderr, "rung: jit: %s\n", error.what());
        std::abort();
    }
    function.jit_status = JitStatus::Compiled;
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    compile_ns_ += ns;
    code_bytes_ += bytes;
    ++compiled_;
    if (log_ != nullptr) {
        std::fprintf(log_,
                     "[jit] compiled %s: %zu bytecode instructions -> %zu machine instructions "
                     "(%zu bytes) in %llu ns\n",
                     function_name(function).c_str(), chunk.code.size(), code.words.size(),
                     bytes, static_cast<unsigned long long>(ns));
    }
}

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
    return "jit: " + std::to_string(compiled_) + " functions compiled (" +
           std::to_string(code_bytes_) + " bytes of code), " + std::to_string(rejected_) +
           " rejected, " + std::to_string(bailouts_) + " bail-outs, " +
           std::to_string(compile_ns_) + " ns compiling\n";
}

}  // namespace rung::jit
