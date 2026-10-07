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

#include "engine.h"
#include "jit/exec_memory.h"
#include "jit/jit_compiler.h"
#include "output.h"
#include "parser.h"
#include "resolver.h"
#include "runtime/function.h"

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
                  bool inline_cache = false) {
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
