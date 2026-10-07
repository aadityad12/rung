// The baseline JIT end to end (notes D16): machine code run directly on a register file, and the
// jit engine's hotness, logging and bail-outs. Built only where the JIT is (RUNG_JIT: arm64 and
// NaN-boxed values).
#include <doctest.h>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "compiler_reg.h"
#include "engine.h"
#include "jit/exec_memory.h"
#include "jit/jit.h"
#include "jit/jit_compiler.h"
#include "output.h"
#include "parser.h"
#include "resolver.h"
#include "runtime/function.h"
#include "runtime/heap.h"
#include "vm_reg.h"

using namespace rung;

namespace {

constexpr std::int32_t kMin = std::numeric_limits<std::int32_t>::min();
constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();

// One function's machine code, callable like the VM calls it.
struct Compiled {
    jit::ExecBuffer buffer{4096};
    JitEntry entry = nullptr;

    explicit Compiled(const RegChunk& chunk) {
        REQUIRE_FALSE(jit::check_whitelist(chunk).has_value());
        jit::GeneratedCode code = jit::generate(chunk);
        buffer = jit::ExecBuffer(code.words.size() * 4);
        buffer.write(code.words.data(), code.words.size());
        entry = buffer.entry<JitEntry>();
    }
};

// A chunk with one binary instruction `r2 = r0 OP r1`, then `return r2`.
RegChunk binary(RegOp op) {
    RegChunk chunk;
    chunk.emit(make_abc(op, 2, 0, 1), 1);
    chunk.emit(make_abc(RegOp::Return, 0, 2, 0), 1);
    chunk.frame_size = 3;
    return chunk;
}

// Runs `code` on registers {a, b, <marker>}; returns the resume index, and the registers.
struct Ran {
    std::uint32_t resume;
    std::vector<Value> regs;
};
Ran run(const Compiled& code, Value a, Value b) {
    std::vector<Value> regs = {a, b, make_int(-12345)};
    std::uint32_t resume = code.entry(regs.data());
    return {resume, regs};
}

std::int32_t int_result(const Compiled& code, std::int32_t a, std::int32_t b) {
    Ran r = run(code, make_int(a), make_int(b));
    REQUIRE(r.resume == 1);  // ran to the RETURN
    REQUIRE(is_int(r.regs[2]));
    return as_int(r.regs[2]);
}

struct EngineRun {
    std::string output;
    std::string log;
    std::string stats;
    EngineResult result;
    std::optional<std::uint64_t> compile_ns;
};

std::string slurp(std::FILE* file) {
    std::string text;
    std::rewind(file);
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, n);
    return text;
}

EngineRun run_jit(const std::string& source, std::uint32_t threshold,
                  bool inline_cache = false, bool superinstructions = false) {
    ParseResult parsed = parse(source);
    REQUIRE(parsed.ok());
    REQUIRE_FALSE(resolve(*parsed.program).has_value());
    std::FILE* out_file = std::tmpfile();
    std::FILE* log_file = std::tmpfile();
    EngineRun run;
    {
        Heap heap;
        Output out(out_file);
        EngineOptions options;
        options.jit_threshold = threshold;
        options.jit_log = log_file;
        options.inline_cache = inline_cache;
        options.superinstructions = superinstructions;
        std::unique_ptr<Engine> engine = make_engine("jit", heap, out, options);
        REQUIRE(engine != nullptr);
        CHECK(engine->name() == "jit");
        run.result = engine->run(*parsed.program);
        out.flush();
        run.stats = engine->stats_report();
        run.compile_ns = engine->jit_compile_ns();
    }
    run.output = slurp(out_file);
    run.log = slurp(log_file);
    std::fclose(out_file);
    std::fclose(log_file);
    return run;
}

std::size_t count(const std::string& text, const std::string& what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
        ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("jit code: int arithmetic wraps at 32 bits like the VM (notes D1, §2.1)") {
    Compiled add(binary(RegOp::Add));
    Compiled sub(binary(RegOp::Sub));
    Compiled mul(binary(RegOp::Mul));
    Compiled div(binary(RegOp::Div));
    Compiled mod(binary(RegOp::Mod));
    CHECK(int_result(add, 2, 3) == 5);
    CHECK(int_result(add, kMax, 1) == kMin);
    CHECK(int_result(add, -7, 3) == -4);
    CHECK(int_result(sub, kMin, 1) == kMax);
    CHECK(int_result(mul, 65536, 65536) == 0);
    CHECK(int_result(mul, -3, 7) == -21);
    CHECK(int_result(div, 7, 2) == 3);
    CHECK(int_result(div, -7, 2) == -3);  // truncates toward zero
    CHECK(int_result(div, kMin, -1) == kMin);
    CHECK(int_result(mod, -7, 3) == -1);  // sign follows the left operand
    CHECK(int_result(mod, 7, -3) == 1);
    CHECK(int_result(mod, kMin, -1) == 0);
}

TEST_CASE("jit code: comparisons produce the VM's bools") {
    struct Case {
        RegOp op;
        std::int32_t a, b;
        bool expected;
    };
    const Case cases[] = {
        {RegOp::Lt, 1, 2, true},   {RegOp::Lt, 2, 2, false},  {RegOp::Lt, -1, 0, true},
        {RegOp::Le, 2, 2, true},   {RegOp::Le, 3, 2, false},  {RegOp::Gt, 3, 2, true},
        {RegOp::Gt, kMin, kMax, false}, {RegOp::Ge, 2, 2, true}, {RegOp::Eq, 5, 5, true},
        {RegOp::Eq, 5, 6, false},  {RegOp::Ne, 5, 6, true},   {RegOp::Ne, 5, 5, false},
    };
    for (const Case& c : cases) {
        Compiled code(binary(c.op));
        Ran r = run(code, make_int(c.a), make_int(c.b));
        CHECK(r.resume == 1);
        REQUIRE(is_bool(r.regs[2]));
        CHECK(as_bool(r.regs[2]) == c.expected);
    }
}

TEST_CASE("jit code: a failed type guard bails out before writing anything") {
    Compiled add(binary(RegOp::Add));
    const Value not_ints[] = {make_float(1.5), make_nil(), make_bool(true), make_float(0.0)};
    for (Value v : not_ints) {
        for (int side = 0; side < 2; ++side) {
            Ran r = side == 0 ? run(add, v, make_int(1)) : run(add, make_int(1), v);
            CHECK(r.resume == 0);  // resume at the ADD itself
            CHECK(as_int(r.regs[2]) == -12345);  // destination untouched
        }
    }
}

TEST_CASE("jit code: a zero divisor bails out to the VM instead of SDIV's silent 0") {
    for (RegOp op : {RegOp::Div, RegOp::Mod}) {
        Compiled code(binary(op));
        Ran r = run(code, make_int(10), make_int(0));
        CHECK(r.resume == 0);
        CHECK(as_int(r.regs[2]) == -12345);
    }
}

TEST_CASE("jit code: constant divisors need no check, unless they are zero") {
    for (std::int32_t divisor : {0, 3}) {
        RegChunk chunk;
        std::size_t k = chunk.add_constant(make_int(divisor));
        chunk.emit(make_abc(RegOp::Div, 2, 0, static_cast<std::uint32_t>(k), kFlagCConst), 1);
        chunk.emit(make_abc(RegOp::Return, 0, 2, 0), 1);
        chunk.frame_size = 3;
        Compiled code(chunk);
        Ran r = run(code, make_int(10), make_nil());
        if (divisor == 0) {
            CHECK(r.resume == 0);
            CHECK(as_int(r.regs[2]) == -12345);
        } else {
            CHECK(r.resume == 1);
            CHECK(as_int(r.regs[2]) == 3);
        }
    }
}

TEST_CASE("jit code: a loop runs to its return") {
    // r1 = 0; r2 = 0; while (r2 < r0) { r1 = r1 + r2; r2 = r2 + 1 } return r1
    RegChunk chunk;
    std::size_t zero = chunk.add_constant(make_int(0));
    std::size_t one = chunk.add_constant(make_int(1));
    chunk.emit(make_abx(RegOp::LoadK, 1, static_cast<std::uint32_t>(zero)), 1);      // 0
    chunk.emit(make_abx(RegOp::LoadK, 2, static_cast<std::uint32_t>(zero)), 1);      // 1
    chunk.emit(make_abc(RegOp::Lt, 3, 2, 0), 2);                                     // 2
    chunk.emit(make_asbx(RegOp::JumpIfFalse, 3, 3), 2);                              // 3 -> 7
    chunk.emit(make_abc(RegOp::Add, 1, 1, 2), 3);                                    // 4
    chunk.emit(make_abc(RegOp::Add, 2, 2, static_cast<std::uint32_t>(one), kFlagCConst), 3);
    chunk.emit(make_asbx(RegOp::Jump, 0, -5), 3);                                    // 6 -> 2
    chunk.emit(make_abc(RegOp::Return, 0, 1, 0), 4);                                 // 7
    chunk.frame_size = 4;
    Compiled code(chunk);
    std::vector<Value> regs(4, make_nil());
    regs[0] = make_int(100);
    CHECK(code.entry(regs.data()) == 7);
    CHECK(as_int(regs[1]) == 4950);
    CHECK(as_int(regs[2]) == 100);
    // A float bound fails the LT's guard on the first test: resume there, nothing else changed.
    regs[0] = make_float(3.5);
    CHECK(code.entry(regs.data()) == 2);
    CHECK(as_int(regs[1]) == 0);
}

TEST_CASE("jit engine: loop_sum is compiled and gives the VM's result") {
    const std::string source = R"(
        fn run() {
            let sum = 0;
            let i = 0;
            while (i < 100000) {
                sum = (sum + i) % 1000007;
                i = i + 1;
            }
            return sum;
        }
        print run();
        print run();
    )";
    EngineRun r = run_jit(source, 1000);
    CHECK(r.result.ok());
    // The sum of 0..99999, reduced mod 1000007 at every step (checked against Python).
    CHECK(r.output == "915007\n915007\n");
    // Hot from its loop's back-edges during the first call; machine code from the second.
    CHECK(r.log.find("[jit] compiled run: 10 bytecode instructions") == 0);
    CHECK(count(r.log, "bail-out") == 0);
    CHECK(r.stats.find("jit: 1 functions compiled") == 0);
    // Bench mode's JSON will report this (notes D16); compiling takes some time.
    REQUIRE(r.compile_ns.has_value());
    CHECK(*r.compile_ns > 0);
}

TEST_CASE("jit engine: the register engine has no JIT and no compile time") {
    Heap heap;
    Output out;
    std::unique_ptr<Engine> engine = make_engine("register", heap, out);
    CHECK(engine->name() == "register");
    CHECK_FALSE(engine->jit_compile_ns().has_value());
}

TEST_CASE("jit engine: runs with the register VM's inline cache") {
    // `step` is compiled; `run` reads the global `step` through the cache and stays in the VM.
    const std::string source = R"(
        fn step(x) { return x * 3 % 7; }
        fn run(n) {
            let x = 1;
            let i = 0;
            while (i < n) { x = step(x); i = i + 1; }
            return x;
        }
        print run(10);
        print run(11);
    )";
    EngineRun r = run_jit(source, 1, true);
    CHECK(r.result.ok());
    CHECK(r.output == "4\n5\n");
    CHECK(count(r.log, "[jit] compiled step") == 1);
    CHECK(count(r.log, "[jit] rejected run") == 1);
#if RUNG_VM_COUNTERS
    CHECK(r.stats.find("inline cache:") != std::string::npos);
#endif
}

TEST_CASE("jit engine: with superinstructions, either half of a fused pair can bail out") {
    // Instructions 4 and 5 are one fused MOD + ADD word pair; 2 and 3 are LT + JUMP_IF_FALSE.
    const std::string source = R"(fn f(a, d, n) {
  let s = 0;
  let i = 0;
  while (i < n) {
    s = i % d + a;
    i = i + 1;
  }
  return s;
}
print f(2, 5, 6);
print f(0.5, 5, 6);
print f(2, 4, 6.5);
print f(2, 0, 6);
)";
    EngineRun r = run_jit(source, 1, false, true);
    CHECK(r.output == "2\n0.5\n4\n");
    REQUIRE(r.result.runtime_error.has_value());
    CHECK(r.result.runtime_error->line == 5);
    CHECK(r.result.runtime_error->message == "division by zero");
    CHECK(count(r.log, "[jit] compiled f") == 1);
    // The float `a`: the second half (a plain ADD word) fails; the VM resumes there.
    CHECK(count(r.log, "bail-out in f at instruction 5 (ADD, line 5)") == 1);
    // The float `n`: the first half of the fused compare-and-branch fails.
    CHECK(count(r.log, "bail-out in f at instruction 2 (LT_JUMP_IF_FALSE, line 4)") == 1);
    // The zero divisor: the first half of MOD + ADD.
    CHECK(count(r.log, "bail-out in f at instruction 4 (MOD_ADD, line 5)") == 1);
}

TEST_CASE("jit engine: a function is compiled when its calls reach the threshold") {
    const std::string source = R"(
        fn add(a, b) { return a + b; }
        let i = 0;
        while (i < 5) { print add(i, 1); i = i + 1; }
    )";
    EngineRun r = run_jit(source, 3);
    CHECK(r.result.ok());
    CHECK(r.output == "1\n2\n3\n4\n5\n");
    CHECK(count(r.log, "[jit] compiled add") == 1);
    // The script's own loop never compiles anything: it runs once (no on-stack replacement).
    CHECK(count(r.log, "<script>") == 0);
}

TEST_CASE("jit engine: rejected functions are logged once and stay in the VM") {
    const std::string source = R"(
        fn fib(n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }
        print fib(15);
    )";
    EngineRun r = run_jit(source, 1);
    CHECK(r.output == "610\n");
    CHECK(r.log == "[jit] rejected fib: instruction 3 (GET_GLOBAL) uses a global variable\n");
    CHECK(r.stats.find("jit: 0 functions compiled (0 bytes of code), 1 rejected, 0 bail-outs") ==
          0);
}

TEST_CASE("jit engine: a guard failing mid-loop resumes the VM on the frame the code left") {
    const std::string source = R"(
        fn f(a, n) {
            let s = 0;
            let i = 0;
            while (i < n) {
                if (i == 3) s = s + a; else s = s + 1;
                i = i + 1;
            }
            return s;
        }
        print f(1, 6);
        print f(0.5, 6);
        print f(2, 6);
    )";
    EngineRun r = run_jit(source, 1);
    CHECK(r.result.ok());
    CHECK(r.output == "6\n5.5\n7\n");
    CHECK(count(r.log, "[jit] compiled f") == 1);
    CHECK(count(r.log, "[jit] bail-out in f") == 1);
    CHECK(r.log.find("(ADD, line 6)") != std::string::npos);
}

TEST_CASE("jit engine: division by zero in machine code is the VM's error, on the right line") {
    const std::string source =
        "fn d(a, b) {\n  return a\n    / b;\n}\nprint d(7, 2);\nprint d(1, 0);\n";
    EngineRun r = run_jit(source, 1);
    CHECK(r.output == "3\n");
    REQUIRE(r.result.runtime_error.has_value());
    CHECK(r.result.runtime_error->line == 3);
    CHECK(r.result.runtime_error->message == "division by zero");
    CHECK(count(r.log, "bail-out in d at instruction 0 (DIV, line 3)") == 1);
}

// ---- background compilation (Engine 5, notes D8) ---------------------------------------------

namespace {

// The register-compiled function called `name`, found through the script's constants.
ObjFunction* find_function(ObjFunction* in, const std::string& name) {
    for (Value constant : in->reg.constants) {
        if (!is_function(constant)) continue;
        ObjFunction* function = as_function(constant);
        if (function->name != nullptr && function->name->chars == name) return function;
        if (ObjFunction* nested = find_function(function, name)) return nested;
    }
    return nullptr;
}

std::unique_ptr<Program> parse_and_resolve(const std::string& source) {
    ParseResult parsed = parse(source);
    REQUIRE(parsed.ok());
    REQUIRE_FALSE(resolve(*parsed.program).has_value());
    return std::move(parsed.program);
}

// `sum_k(n)` returns 0 + k + 2k + ... + (n-1)k, and is inside the JIT's whitelist.
std::string summing_functions(int count) {
    std::string source;
    for (int k = 0; k < count; ++k) {
        source += "fn sum_" + std::to_string(k) +
                  "(n) { let s = 0; let i = 0; while (i < n) { s = s + i * " +
                  std::to_string(k) + "; i = i + 1; } return s; }\n";
    }
    return source;
}

// Runs `function`'s published machine code on a fresh frame with `n` as its argument and returns
// what it returned, the way the VM would: the code stops at the RETURN, which names the register.
std::int32_t run_published(ObjFunction& function, JitEntry entry, std::int32_t n) {
    std::vector<Value> registers(static_cast<std::size_t>(function.reg.frame_size), make_nil());
    registers[0] = make_int(n);
    const Instruction at = function.reg.code[entry(registers.data())];
    REQUIRE(insn_op(at) == RegOp::Return);
    REQUIRE(is_int(registers[insn_b(at)]));
    return as_int(registers[insn_b(at)]);
}

}  // namespace

TEST_CASE("jit background: hot functions compile on the compiler thread and the VM enters them") {
    // f is queued on its first call (threshold 1) and runs in the VM meanwhile. After the
    // compiler thread is done, calling it through `float_call` enters the machine code, whose
    // guard fails on the float: the logged bail-out proves the published code ran.
    const std::string source = R"(
        fn f(a, n) { let s = 0; let i = 0; while (i < n) { s = s + a; i = i + 1; } return s; }
        fn int_call() { return f(2, 5); }
        fn float_call() { return f(0.5, 3); }
        print f(1, 3);
    )";
    std::unique_ptr<Program> program = parse_and_resolve(source);
    std::FILE* out_file = std::tmpfile();
    std::FILE* log_file = std::tmpfile();
    {
        Heap heap;
        Output out(out_file);
        EngineOptions options;
        options.jit_threshold = 1;
        options.jit_log = log_file;
        options.jit_background = true;
        RegisterEngine engine(heap, out, options);
        CHECK(engine.name() == "jit");
        REQUIRE(engine.run(*program).ok());
        REQUIRE(engine.jit() != nullptr);
        engine.jit()->wait_until_idle();
        CHECK(engine.jit()->functions_compiled() == 1);  // f; int_call and float_call never ran
        CallResult ints = engine.call_global("int_call");
        REQUIRE(ints.ok());
        CHECK(as_int(ints.value) == 10);
        CallResult floats = engine.call_global("float_call");
        REQUIRE(floats.ok());
        CHECK(as_float(floats.value) == 1.5);
        engine.jit()->wait_until_idle();  // int_call and float_call were queued, then rejected
        CHECK(engine.jit()->functions_rejected() == 2);
        CHECK(engine.stats_report().find("on the compiler thread, 0 queued or in progress") !=
              std::string::npos);
        out.flush();
    }
    CHECK(slurp(out_file) == "3\n");
    const std::string log = slurp(log_file);
    CHECK(count(log, "[jit] compiled f: ") == 1);
    CHECK(count(log, " on the compiler thread\n") == 3);
    CHECK(count(log, "[jit] bail-out in f") == 1);
    std::fclose(out_file);
    std::fclose(log_file);
}

TEST_CASE("jit background: a queued function is a GC root until the compiler has published it") {
    std::unique_ptr<Program> program = parse_and_resolve(summing_functions(1));
    Heap heap;
    jit::Jit jit(1, nullptr, true);
    Heap::RootHandle roots = heap.add_root_marker([&jit](Heap& h) { jit.mark_pending(h); });
    RegCompileResult compiled = compile_register(*program, heap);
    REQUIRE(compiled.ok());
    ObjFunction* function = find_function(compiled.function, "sum_0");
    REQUIRE(function != nullptr);

    jit.hold_for_testing(true);  // keep it queued while the collector runs
    jit.enqueue(*function);
    CHECK(function->jit_status == JitStatus::Queued);
    // Nothing else reaches the function: the script, its only owner, is not rooted, so this
    // collection frees the script, and would free the function too without mark_pending.
    heap.collect();
    const std::size_t live_while_queued = heap.stats().live_objects;
    CHECK(live_while_queued >= 2);  // the function and its name, at least

    jit.hold_for_testing(false);
    jit.wait_until_idle();
    // Run the code the compiler published (had the collection freed the function, ASan would
    // report a use after free on the compiler thread or here).
    const JitEntry entry = function->jit_entry.load(std::memory_order_acquire);
    REQUIRE(entry != nullptr);
    jit::Jit::adopt(*function, entry);
    CHECK(function->jit_status == JitStatus::Compiled);
    CHECK(run_published(*function, entry, 10) == 0);

    // Published: no longer a root, so the next collection frees it.
    heap.collect();
    CHECK(heap.stats().live_objects < live_while_queued);
    heap.remove_root_marker(roots);
}

TEST_CASE("jit background: a rejected function stays queued, in the VM, with no code") {
    std::unique_ptr<Program> program = parse_and_resolve("fn g() { return g; }\n");
    Heap heap;
    jit::Jit jit(1, nullptr, true);
    RegCompileResult compiled = compile_register(*program, heap);
    REQUIRE(compiled.ok());
    ObjFunction* function = find_function(compiled.function, "g");
    REQUIRE(function != nullptr);
    jit.enqueue(*function);
    jit.wait_until_idle();
    CHECK(jit.functions_rejected() == 1);
    CHECK(function->jit_entry.load(std::memory_order_acquire) == nullptr);
    CHECK(function->jit_status == JitStatus::Queued);  // the compiler never writes the status
}

TEST_CASE("jit background: shutdown drops what is still queued and joins the thread") {
    std::unique_ptr<Program> program = parse_and_resolve(summing_functions(3));
    Heap heap;
    RegCompileResult compiled = compile_register(*program, heap);
    REQUIRE(compiled.ok());
    std::FILE* log_file = std::tmpfile();
    {
        jit::Jit jit(1, log_file, true);
        jit.hold_for_testing(true);
        for (const char* name : {"sum_0", "sum_1", "sum_2"}) {
            ObjFunction* function = find_function(compiled.function, name);
            REQUIRE(function != nullptr);
            jit.enqueue(*function);
        }
        jit.shutdown();
        CHECK(jit.functions_compiled() == 0);
        jit.shutdown();  // idempotent; the destructor calls it once more
    }
    const std::string log = slurp(log_file);
    CHECK(count(log, "[jit] cancelled sum_") == 3);
    CHECK(count(log, "[jit] compiled") == 0);
    std::fclose(log_file);
}

TEST_CASE("jit background: code is published while the collector runs and the VM's thread calls "
          "it") {
    // The race ThreadSanitizer is here for (the tsan-goto-nanbox preset): the compiler thread
    // reads queued functions and publishes their code while this thread collects garbage every
    // round (marking those same functions) and calls whatever has been published so far.
    constexpr int kFunctions = 40;
    std::unique_ptr<Program> program = parse_and_resolve(summing_functions(kFunctions));
    Heap heap;
    jit::Jit jit(1, nullptr, true);
    Heap::RootHandle roots = heap.add_root_marker([&jit](Heap& h) { jit.mark_pending(h); });
    RegCompileResult compiled = compile_register(*program, heap);
    REQUIRE(compiled.ok());
    std::vector<ObjFunction*> functions;
    for (int k = 0; k < kFunctions; ++k) {
        functions.push_back(find_function(compiled.function, "sum_" + std::to_string(k)));
        REQUIRE(functions.back() != nullptr);
    }
    {
        // Keeps the script, and so every function, alive through the collections below.
        Heap::TempRoot keep(heap, make_obj(compiled.function));
        for (ObjFunction* function : functions) jit.enqueue(*function);
        std::vector<bool> ran(kFunctions, false);
        int remaining = kFunctions;
        while (remaining > 0) {
            heap.collect();
            for (std::size_t k = 0; k < functions.size(); ++k) {
                if (ran[k]) continue;
                ObjFunction& function = *functions[k];
                const JitEntry entry = function.jit_entry.load(std::memory_order_acquire);
                if (entry == nullptr) continue;
                if (function.jit_status != JitStatus::Compiled) {
                    jit::Jit::adopt(function, entry);
                }
                CHECK(run_published(function, entry, 100) == 4950 * static_cast<int>(k));
                ran[k] = true;
                --remaining;
            }
        }
    }
    CHECK(jit.functions_compiled() == kFunctions);
    jit.shutdown();
    heap.remove_root_marker(roots);
}
