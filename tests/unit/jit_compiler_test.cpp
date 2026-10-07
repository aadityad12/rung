// The JIT's whitelist decision (notes D16), on small functions. It only inspects bytecode, so
// these run on every platform, with or without the JIT.
#include <doctest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compiler_reg.h"
#include "jit/jit_compiler.h"
#include "parser.h"
#include "resolver.h"
#include "runtime/function.h"
#include "runtime/heap.h"

using namespace rung;
using rung::jit::unfused_op;

namespace {

const ObjFunction* find_function(const ObjFunction& in, std::string_view name) {
    for (Value constant : in.reg.constants) {
        if (!is_function(constant)) continue;
        const ObjFunction* fn = as_function(constant);
        if (fn->name != nullptr && fn->name->chars == name) return fn;
        if (const ObjFunction* nested = find_function(*fn, name)) return nested;
    }
    return nullptr;
}

// What check_whitelist says about the function `name` in `source`: "ok", or
// "<instruction index>: <reason>". With `superinstructions` the code is fused first (rung 3d).
std::string verdict(const std::string& source, std::string_view name,
                    bool superinstructions = false) {
    ParseResult parsed = parse(source);
    REQUIRE(parsed.ok());
    REQUIRE_FALSE(resolve(*parsed.program).has_value());
    Heap heap;
    RegCompileOptions options;
    options.superinstructions = superinstructions;
    RegCompileResult compiled = compile_register(*parsed.program, heap, options);
    REQUIRE(compiled.ok());
    Heap::TempRoot root(heap, make_obj(compiled.function));
    const ObjFunction* fn = find_function(*compiled.function, name);
    REQUIRE(fn != nullptr);
    std::optional<jit::Rejection> rejection = jit::check_whitelist(fn->reg);
    if (!rejection) return "ok";
    return std::to_string(rejection->index) + ": " + rejection->reason;
}

}  // namespace

TEST_CASE("jit whitelist: int arithmetic, comparisons, loops and returns compile") {
    CHECK(verdict("fn add(a, b) { return a + b; }", "add") == "ok");
    CHECK(verdict(R"(
        fn run() {
            let sum = 0;
            let i = 0;
            while (i < 1000000) { sum = (sum + i) % 1000007; i = i + 1; }
            return sum;
        })", "run") == "ok");
    CHECK(verdict(R"(
        fn all(a, b) {
            let x = -a * b / 3 - 4;
            if (a == b or a != 1 and a <= b and a >= 0 and a > -5) x = x + 1;
            for (let i = 0; i < 3; i = i + 1) x = x % 7;
            return x;
        })", "all") == "ok");
    // Falling off the end, nil and bool loads, register moves.
    CHECK(verdict("fn f(a) { let t = true; let f = false; let n = nil; let c = a; }", "f") ==
          "ok");
    // A nil or bool constant may be returned; the VM performs the return itself.
    CHECK(verdict("fn f() { return nil; }", "f") == "ok");
    CHECK(verdict("fn f() { return true; }", "f") == "ok");
}

TEST_CASE("jit whitelist: calls, globals, closures, strings, floats, arrays and print are out") {
    // fib calls itself through a global (notes D7): the global read is the first thing out.
    CHECK(verdict("fn fib(n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }", "fib") ==
          "3: uses a global variable");
    CHECK(verdict("fn f() { return clock; }", "f") == "0: uses a global variable");
    CHECK(verdict("fn f(g) { return g(); }", "f") == "1: calls a function");
    CHECK(verdict("fn f(a) { return a + 1.5; }", "f") == "0: uses a float constant");
    CHECK(verdict("fn f() { let x = 2.5; return x; }", "f") == "0: uses a float constant");
    CHECK(verdict(R"(fn f(a) { return a + "s"; })", "f") == "0: uses a string constant");
    CHECK(verdict(R"(fn f() { return "s"; })", "f") == "0: uses a string constant");
    CHECK(verdict("fn f(a) { print a; }", "f") == "0: prints");
    CHECK(verdict("fn f(a) { return a[0]; }", "f") == "0: uses an array");
    CHECK(verdict("fn f(a) { a[0] = 1; }", "f") == "0: uses an array");
    CHECK(verdict("fn f(a) { return [a]; }", "f") == "1: uses an array");
    CHECK(verdict("fn f(a) { return !a; }", "f") == "0: uses '!', which is not in the whitelist");
    CHECK(verdict("fn f(a) { return a == nil; }", "f") ==
          "0: has a nil or bool constant as an arithmetic operand");
    CHECK(verdict("fn f(a) { fn g() { return a; } return 1; }", "f") ==
          "0: creates or closes a closure");
    CHECK(verdict("fn f(a) { fn g() { return a; } return 1; }", "g") ==
          "0: uses a captured variable");
}

TEST_CASE("jit whitelist: one instruction outside it rejects the whole function") {
    // Everything before the print is fine; the print still keeps the function in the VM.
    CHECK(verdict(R"(
        fn f(n) {
            let s = 0;
            while (s < n) s = s + 1;
            print s;
            return s;
        })", "f").find(": prints") != std::string::npos);
}

TEST_CASE("jit whitelist: fused superinstructions are judged as their two halves") {
    const std::string loop = R"(
        fn run(n, d) {
            let s = 0;
            let i = 0;
            while (i < n) { s = i % d + s / d; i = i + 1; }
            return s;
        })";
    CHECK(verdict(loop, "run", true) == "ok");
    // IndexSet + Add fuses; the rejection names the first half, as without fusion.
    const std::string array = "fn f(a, i) { a[i] = 1; return i + 1; }";
    CHECK(verdict(array, "f", true) == verdict(array, "f", false));
    CHECK(verdict(array, "f", true) == "0: uses an array");
    CHECK(unfused_op(make_abc(RegOp::ModAdd, 0, 0, 0)) == RegOp::Mod);
    CHECK(unfused_op(make_abc(RegOp::Add, 0, 0, 0)) == RegOp::Add);
}

TEST_CASE("jit whitelist: frames past LDR/STR's reach are rejected") {
    RegChunk chunk;
    chunk.emit(make_abc(RegOp::ReturnNil, 0, 0, 0), 1);
    chunk.frame_size = jit::kMaxFrameSize;
    CHECK_FALSE(jit::check_whitelist(chunk).has_value());
    chunk.frame_size = jit::kMaxFrameSize + 1;
    std::optional<jit::Rejection> rejection = jit::check_whitelist(chunk);
    REQUIRE(rejection.has_value());
    CHECK(rejection->reason == "needs 4097 registers, more than the JIT addresses (4096)");
}

#if RUNG_NANBOX
TEST_CASE("jit codegen: fused code compiles to exactly the machine code of unfused code") {
    // Fusion changes only opcode bytes (register_code.h, "Fusion"), and the JIT reads each word
    // as its unfused instruction, so the words must be identical.
    const std::string source = R"(
        fn run(n, d) {
            let s = 0;
            let i = 0;
            while (i <= n) { s = i % d + s / d; i = i + 1; }
            while (i < n + n) { s = s + i; i = i + 1; }
            return s;
        })";
    ParseResult parsed = parse(source);
    REQUIRE(parsed.ok());
    REQUIRE_FALSE(resolve(*parsed.program).has_value());
    std::vector<std::uint32_t> words[2];
    std::size_t fused_words = 0;
    for (int fuse = 0; fuse < 2; ++fuse) {
        Heap heap;
        RegCompileOptions options;
        options.superinstructions = fuse == 1;
        RegCompileResult compiled = compile_register(*parsed.program, heap, options);
        REQUIRE(compiled.ok());
        Heap::TempRoot root(heap, make_obj(compiled.function));
        const ObjFunction* fn = find_function(*compiled.function, "run");
        REQUIRE(fn != nullptr);
        if (fuse == 1) {
            for (Instruction insn : fn->reg.code) fused_words += is_fused(insn_op(insn)) ? 1 : 0;
        }
        REQUIRE_FALSE(jit::check_whitelist(fn->reg).has_value());
        words[fuse] = jit::generate(fn->reg).words;
    }
    CHECK(fused_words >= 4);  // LT and LE + JUMP_IF_FALSE, MOD + ADD, ADD + JUMP
    CHECK(words[0] == words[1]);
}

TEST_CASE("jit codegen: every bytecode instruction is mapped to its machine code") {
    RegChunk chunk;
    // r1 = r0 + r0; if r1 is falsy skip; return r1; return nil
    chunk.emit(make_abc(RegOp::Add, 1, 0, 0), 1);
    chunk.emit(make_asbx(RegOp::JumpIfFalse, 1, 1), 1);
    chunk.emit(make_abc(RegOp::Return, 0, 1, 0), 2);
    chunk.emit(make_abc(RegOp::ReturnNil, 0, 0, 0), 3);
    chunk.frame_size = 2;
    REQUIRE_FALSE(jit::check_whitelist(chunk).has_value());
    jit::GeneratedCode code = jit::generate(chunk);
    REQUIRE(code.starts.size() == chunk.code.size());
    // 4 constant set-ups first, then instructions in order, then the bail-out exits.
    CHECK(code.starts[0] == 4);
    for (std::size_t i = 1; i < code.starts.size(); ++i) {
        CHECK(code.starts[i] > code.starts[i - 1]);
    }
    CHECK(code.starts.back() < code.words.size());
    // The last two words are the ADD's bail-out: `mov w0, #0; ret`.
    CHECK(code.words[code.words.size() - 2] == 0x52800000u);  // movz w0, #0
    CHECK(code.words.back() == 0xD65F03C0u);                    // ret
}
#endif
