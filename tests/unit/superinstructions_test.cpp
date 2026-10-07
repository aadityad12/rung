#include <doctest.h>

#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "bytecode/register_code.h"
#include "compiler_reg.h"
#include "disassembler.h"
#include "engine.h"
#include "output.h"
#include "parser.h"
#include "resolver.h"
#include "superinstructions.h"
#include "vm_reg.h"
#include "vm_stack.h"

using namespace rung;

namespace {

// ---- helpers ---------------------------------------------------------------------------------

std::unique_ptr<Program> resolved(const std::string& source) {
    ParseResult parsed = parse(source);
    REQUIRE_MESSAGE(parsed.ok(), parsed.error->message);
    auto error = resolve(*parsed.program);
    REQUIRE_MESSAGE(!error.has_value(), error->message);
    return std::move(parsed.program);
}

std::string slurp_file(std::FILE* file) {
    std::string text;
    std::rewind(file);
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, n);
    return text;
}

struct Run {
    std::string output;
    EngineResult result;
    std::string stats;
    std::string pairs;
#if RUNG_VM_COUNTERS
    std::uint64_t dispatched = 0;
    std::uint64_t fused_dispatched = 0;  // dispatches of superinstructions
#endif
};

Run run_register(const std::string& source, bool superinstructions, bool stress = false,
                 bool inline_cache = false) {
    std::unique_ptr<Program> program = resolved(source);
    std::FILE* file = std::tmpfile();
    Run run;
    {
        Heap heap;
        heap.set_stress(stress);
        Output out(file);
        EngineOptions options;
        options.superinstructions = superinstructions;
        options.inline_cache = inline_cache;
        std::unique_ptr<Engine> engine = make_engine("register", heap, out, options);
        REQUIRE(engine != nullptr);
        run.result = engine->run(*program);
        out.flush();
        run.stats = engine->stats_report();
        run.pairs = engine->pair_report();
#if RUNG_VM_COUNTERS
        const RegVmCounters& counters = static_cast<RegisterEngine&>(*engine).counters();
        run.dispatched = counters.instructions();
        for (const FusedPair& pair : kFusedPairs) {
            run.fused_dispatched += counters.by_opcode[static_cast<std::size_t>(pair.fused)];
        }
#endif
    }
    run.output = slurp_file(file);
    std::fclose(file);
    return run;
}

// What a run shows the outside world: its output and how it ended.
std::string outcome(const Run& run) {
    std::string text = run.output;
    if (run.result.compile_error) {
        text += "compile error line " + std::to_string(run.result.compile_error->line) + ": " +
                run.result.compile_error->message;
    }
    if (run.result.runtime_error) {
        text += "runtime error line " + std::to_string(run.result.runtime_error->line) + ": " +
                run.result.runtime_error->message;
    }
    return text;
}

RegCompileResult compile(const Program& program, Heap& heap, bool superinstructions) {
    RegCompileOptions options;
    options.superinstructions = superinstructions;
    RegCompileResult compiled = compile_register(program, heap, options);
    REQUIRE(compiled.ok());
    return compiled;
}

std::string dis(const std::string& source, bool superinstructions) {
    std::unique_ptr<Program> program = resolved(source);
    Heap heap;
    RegCompileResult compiled = compile(*program, heap, superinstructions);
    Heap::TempRoot root(heap, make_obj(compiled.function));
    return disassemble_register(*compiled.function);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// A chunk holding `code`, every instruction on line 1.
RegChunk chunk_of(const std::vector<Instruction>& code) {
    RegChunk chunk;
    for (Instruction insn : code) chunk.emit(insn, 1);
    return chunk;
}

Instruction with_op(Instruction insn, RegOp op) {
    return (insn & ~Instruction{0xFF}) | static_cast<Instruction>(op);
}

// One program that contains every superinstruction, used where a test needs them all.
const char* kEveryPair = R"(
    fn all(a, n) {
      let i = 0;
      let sum = 0;
      while (i < n) {
        a[i] = i * 2;
        i = i + 1;
        sum = (sum + i % 7) % 1000;
        sum = sum + 10 / 4;
      }
      let j = 0;
      while (j <= 3) {
        j = j + 1;
      }
      return sum + j;
    }
    print all(array(20, 0), 20);
)";

}  // namespace

// ---- the fusion table ------------------------------------------------------------------------

TEST_CASE("fusion table: each superinstruction names its two halves, and only those fuse") {
    std::set<std::string> names;
    for (const FusedPair& pair : kFusedPairs) {
        CHECK(is_fused(pair.fused));
        CHECK_FALSE(is_fused(pair.first));
        CHECK_FALSE(is_fused(pair.second));
        CHECK(fused_first(pair.fused) == pair.first);
        CHECK(fused_second(pair.fused) == pair.second);
        CHECK(fuse_pair(pair.first, pair.second) == pair.fused);
        // The halves are real instructions with names, and so is the fused opcode.
        CHECK(std::string(reg_opcode_name(pair.fused)) != "UNKNOWN");
        CHECK(names.insert(reg_opcode_name(pair.fused)).second);  // no two share a name
    }
    // A pair that is not listed stays itself, in either order.
    CHECK(fuse_pair(RegOp::Add, RegOp::Add) == RegOp::Add);
    CHECK(fuse_pair(RegOp::JumpIfFalse, RegOp::Lt) == RegOp::JumpIfFalse);
    CHECK(fuse_pair(RegOp::Add, RegOp::Jump) == RegOp::AddJump);
    CHECK(fuse_pair(RegOp::Jump, RegOp::Add) == RegOp::Jump);
    // The original opcodes are not superinstructions.
    CHECK_FALSE(is_fused(RegOp::IndexSet));
    CHECK_FALSE(is_fused(RegOp::Move));
}

// ---- the pass --------------------------------------------------------------------------------

TEST_CASE("fuse pass: changes the opcode of the first word and nothing else") {
    Instruction lt = make_abc(RegOp::Lt, 2, 1, 7, kFlagCConst);
    Instruction jump = make_asbx(RegOp::JumpIfFalse, 2, 5);
    RegChunk chunk = chunk_of({lt, jump});
    CHECK(fuse_superinstructions(chunk) == 1);
    REQUIRE(chunk.code.size() == 2);
    CHECK(chunk.code[0] == with_op(lt, RegOp::LtJumpIfFalse));
    CHECK(insn_a(chunk.code[0]) == 2);
    CHECK(insn_b(chunk.code[0]) == 1);
    CHECK(insn_c(chunk.code[0]) == 7);
    CHECK(insn_flags(chunk.code[0]) == kFlagCConst);
    CHECK(chunk.code[1] == jump);  // the second half is untouched, a whole instruction still
}

TEST_CASE("fuse pass: a pair never shares a word with the next pair") {
    // Mod Add Jump: Mod+Add is listed and Add+Jump is listed, but Add is already the second half
    // of the first pair, so Jump stays alone.
    Instruction mod = make_abc(RegOp::Mod, 1, 0, 0);
    Instruction add = make_abc(RegOp::Add, 1, 1, 0);
    Instruction jump = make_asbx(RegOp::Jump, 0, -3);
    RegChunk chunk = chunk_of({mod, add, jump});
    CHECK(fuse_superinstructions(chunk) == 1);
    CHECK(insn_op(chunk.code[0]) == RegOp::ModAdd);
    CHECK(insn_op(chunk.code[1]) == RegOp::Add);
    CHECK(insn_op(chunk.code[2]) == RegOp::Jump);

    // Running the pass again must not turn that Add into the first half of Add+Jump.
    std::vector<Instruction> before = chunk.code;
    CHECK(fuse_superinstructions(chunk) == 0);
    CHECK(chunk.code == before);
}

TEST_CASE("fuse pass: pairs follow each other, and lone or unlisted words are left alone") {
    Instruction lt = make_abc(RegOp::Lt, 2, 0, 1);
    Instruction jif = make_asbx(RegOp::JumpIfFalse, 2, 2);
    Instruction add = make_abc(RegOp::Add, 0, 0, 0);
    Instruction jump = make_asbx(RegOp::Jump, 0, -4);
    RegChunk chunk = chunk_of({lt, jif, add, add, jump, add});
    CHECK(fuse_superinstructions(chunk) == 2);  // Lt+Jif, then (Add) and Add+Jump
    CHECK(insn_op(chunk.code[0]) == RegOp::LtJumpIfFalse);
    CHECK(insn_op(chunk.code[2]) == RegOp::Add);  // Add Add is not listed
    CHECK(insn_op(chunk.code[3]) == RegOp::AddJump);
    CHECK(insn_op(chunk.code[5]) == RegOp::Add);  // last word, nothing after it

    RegChunk empty;
    CHECK(fuse_superinstructions(empty) == 0);
    RegChunk single = chunk_of({lt});
    CHECK(fuse_superinstructions(single) == 0);
    CHECK(single.code[0] == lt);
}

TEST_CASE("fuse pass: a closure's capture words are data and never the half of a pair") {
    Instruction closure = make_abx(RegOp::Closure, 0, 0);
    Instruction capture = make_abc(RegOp::Capture, 1, 3, 0);
    Instruction add = make_abc(RegOp::Add, 0, 0, 0);
    Instruction jump = make_asbx(RegOp::Jump, 0, -4);
    RegChunk chunk = chunk_of({closure, capture, add, jump});
    CHECK(fuse_superinstructions(chunk) == 1);
    CHECK(chunk.code[0] == closure);
    CHECK(chunk.code[1] == capture);
    CHECK(insn_op(chunk.code[2]) == RegOp::AddJump);
}

// ---- the compiler flag -----------------------------------------------------------------------

TEST_CASE("--superinstructions: the code has the same words, and differs only in fused opcodes") {
    std::unique_ptr<Program> program = resolved(kEveryPair);
    Heap plain_heap;
    RegCompileResult plain = compile(*program, plain_heap, false);
    Heap::TempRoot plain_root(plain_heap, make_obj(plain.function));
    Heap fused_heap;
    RegCompileResult fused = compile(*program, fused_heap, true);
    Heap::TempRoot fused_root(fused_heap, make_obj(fused.function));
    CHECK(plain.fused_pairs == 0);
    CHECK(fused.fused_pairs > 0);

    // Compare the nested function `all` word by word: every word is the same except for the
    // opcode byte of the first half of a pair, which is the pair's fused opcode.
    const ObjFunction* plain_all = nullptr;
    const ObjFunction* fused_all = nullptr;
    for (const Value& v : plain.function->reg.constants) {
        if (is_function(v)) plain_all = as_function(v);
    }
    for (const Value& v : fused.function->reg.constants) {
        if (is_function(v)) fused_all = as_function(v);
    }
    REQUIRE(plain_all != nullptr);
    REQUIRE(fused_all != nullptr);
    REQUIRE(plain_all->reg.code.size() == fused_all->reg.code.size());
    std::size_t differing = 0;
    std::set<RegOp> seen;
    for (std::size_t i = 0; i < plain_all->reg.code.size(); ++i) {
        Instruction before = plain_all->reg.code[i];
        Instruction after = fused_all->reg.code[i];
        if (before == after) continue;
        ++differing;
        RegOp op = insn_op(after);
        seen.insert(op);
        CHECK(is_fused(op));
        CHECK(after == with_op(before, op));
        CHECK(fused_first(op) == insn_op(before));
        CHECK(fused_second(op) == insn_op(plain_all->reg.code[i + 1]));
    }
    CHECK(differing > 0);
    // The program was written to contain every superinstruction.
    CHECK(seen.size() == kFusedPairCount);
    // The line table is untouched, so each half keeps reporting its own line.
    CHECK(plain_all->reg.lines.size() == fused_all->reg.lines.size());
}

TEST_CASE("--superinstructions: the disassembly shows the fused name and marks the second half") {
    std::string text = dis("fn f(n) { let i = 0; while (i < n) { i = i + 1; } return i; }\n", true);
    CHECK(contains(text, "LT_JUMP_IF_FALSE r2 r1 r0"));
    CHECK(contains(text, "JUMP_IF_FALSE  r2 -> 0005   ; second half of the row above"));
    CHECK(contains(text, "ADD_JUMP       r1 r1 k1(1)"));
    CHECK(contains(text, "JUMP           -> 0001   ; second half of the row above"));
    // Without the flag there is no fused name and no mark.
    std::string plain =
        dis("fn f(n) { let i = 0; while (i < n) { i = i + 1; } return i; }\n", false);
    CHECK_FALSE(contains(plain, "LT_JUMP_IF_FALSE"));
    CHECK_FALSE(contains(plain, "second half"));
    CHECK(contains(plain, "LT             r2 r1 r0"));
}

TEST_CASE("--superinstructions: functions nested in functions are fused too") {
    std::string text = dis(
        "fn outer() { fn inner(n) { let i = 0; while (i < n) { i = i + 1; } return i; }"
        " return inner; }\n",
        true);
    CHECK(contains(text, "== inner"));
    CHECK(contains(text, "LT_JUMP_IF_FALSE"));
}

// ---- same behaviour with the flag ------------------------------------------------------------

TEST_CASE("superinstructions: a program prints and fails exactly as it does without them") {
    const char* programs[] = {
        kEveryPair,
        // An error in the first half of each pair, then in the second half on a later line.
        "fn f(x, z) { let q = x /\n z; let r = q + 1; return r; }\n"
        "print f(6, 3);\nprint f(6, 0);\n",
        "fn f(x, y) { let m = x % 3; let n = m +\n y; return n; }\n"
        "print f(7, 5);\nprint f(7, nil);\n",
        "fn f(a, i) { a[i] =\n 1; let n = i + 1; return n; }\nprint f(array(2, 0), 1);\n"
        "print f(array(2, 0), 2);\n",
        "fn f(a, c) { a[0] = 1; let b = c +\n nil; return b; }\nprint f(array(1, 0), 5);\n",
        "fn f(x) { if (x <\n nil) return 1; return 2; }\nprint f(3);\n",
        "fn f() { let x = 1; while (x < 3) { x = x + 1; x = x +\n \"s\"; } }\nf();\n",
        // Strings allocate in the second half of a pair; the collector may run there.
        "fn f() { let s = \"\"; let i = 0; while (i < 20) { let m = i % 3; s = s + \"ab\";"
        " i = i + 1; } return s; }\nprint f();\n",
        // Wraparound and float edge cases through the fused arithmetic.
        "fn f() { let x = 2147483647; let i = 0; while (i < 2) { i = i + 1; x = x + 1; }"
        " return x; }\nprint f();\n",
        "fn f(x) { let q = x / 0.0; let r = q + 1; return r; }\nprint f(1);\nprint f(-1);\n",
        "fn f(x) { let q = x / 2; let r = q + 1; return r; }\nprint f(-7);\nprint f(7);\n",
    };
    for (const char* source : programs) {
        for (bool stress : {false, true}) {
            INFO(source);
            Run plain = run_register(source, false, stress);
            Run fused = run_register(source, true, stress);
            CHECK(outcome(fused) == outcome(plain));
        }
    }
}

TEST_CASE("superinstructions: an error in the second half is on the second half's line") {
    // The remainder is on line 2 and the failing addition on line 3.
    const char* source =
        "fn f(x, y) {\n let m = x % 3;\n let n = m + y;\n return n;\n}\nf(7, nil);\n";
    Run run = run_register(source, true);
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 3);
    Run plain = run_register(source, false);
    REQUIRE(plain.result.runtime_error.has_value());
    CHECK(plain.result.runtime_error->line == run.result.runtime_error->line);
    CHECK(plain.result.runtime_error->message == run.result.runtime_error->message);
}

TEST_CASE("superinstructions compose with the inline cache") {
    // Globals are read and written around the fused pairs. The inline cache indexes its slots by
    // instruction, which fusion does not move.
    const char* source = R"(
        let total = 0;
        let step = 3;
        fn bump(n) {
          let i = 0;
          while (i < n) {
            total = total + step % 5;
            i = i + 1;
          }
          return total;
        }
        print bump(10);
        step = 4;
        print bump(10);
    )";
    Run plain = run_register(source, false);
    Run fused = run_register(source, true, false, true);
    Run fused_stress = run_register(source, true, true, true);
    CHECK(plain.output == "30\n70\n");
    CHECK(outcome(fused) == outcome(plain));
    CHECK(outcome(fused_stress) == outcome(plain));
}

// ---- engines, stats and counters -------------------------------------------------------------

TEST_CASE("superinstructions: only the register VM has them") {
    Heap heap;
    std::FILE* file = std::tmpfile();
    Output out(file);
    EngineOptions options;
    options.superinstructions = true;
    CHECK(make_engine("tree", heap, out, options) == nullptr);
    CHECK(make_engine("stack", heap, out, options) == nullptr);
    CHECK(make_engine("register", heap, out, options) != nullptr);
    out.flush();
    std::fclose(file);
}

TEST_CASE("superinstructions: --stats reports how many pairs were fused") {
    Run fused = run_register("fn f(n) { let i = 0; while (i < n) { i = i + 1; } }\nf(3);\n", true);
    CHECK(contains(fused.stats, "superinstructions: pairs fused: 2"));
    Run plain = run_register("fn f(n) { let i = 0; while (i < n) { i = i + 1; } }\nf(3);\n", false);
    CHECK_FALSE(contains(plain.stats, "superinstructions:"));
}

#if RUNG_VM_COUNTERS
TEST_CASE("superinstructions: each fused dispatch replaces exactly two dispatches") {
    Run plain = run_register(kEveryPair, false);
    Run fused = run_register(kEveryPair, true);
    REQUIRE(plain.result.ok());
    REQUIRE(fused.result.ok());
    CHECK(fused.output == plain.output);
    CHECK(fused.fused_dispatched > 0);
    CHECK(plain.fused_dispatched == 0);
    // The fused program dispatches one instruction where the plain one dispatched two, and the
    // same number of everything else, so the difference is exactly the fused dispatches.
    CHECK(plain.dispatched - fused.dispatched == fused.fused_dispatched);
    CHECK(contains(fused.stats, "LT_JUMP_IF_FALSE"));
    CHECK(contains(fused.stats, "MOD_ADD"));
}

TEST_CASE("pair counters: --stats=pairs counts opcode X followed by opcode Y") {
    // ADD r0 k k; PRINT r0; RETURN_NIL. The first instruction follows nothing and is not a pair.
    Run run = run_register("print 1 + 2;\n", false);
    CHECK(contains(run.pairs, "pairs: 2 opcode pairs dispatched, 2 distinct"));
    CHECK(contains(run.pairs, "ADD            -> PRINT"));
    CHECK(contains(run.pairs, "PRINT          -> RETURN_NIL"));

    // A loop: the same pair many times, listed first, with its share.
    Run loop = run_register("let i = 0; while (i < 5) { i = i + 1; }\n", false);
    CHECK(contains(loop.pairs, "LT             -> JUMP_IF_FALSE"));
}

TEST_CASE("pair counters: the stack VM counts pairs too, and every pair adds up") {
    std::unique_ptr<Program> program = resolved("print 1 + 2;\n");
    std::FILE* file = std::tmpfile();
    Heap heap;
    Output out(file);
    StackEngine engine(heap, out);
    REQUIRE(engine.run(*program).ok());
    const VmCounters& counters = engine.counters();
    // CONST CONST ADD PRINT NIL RETURN: six dispatches, five pairs.
    CHECK(counters.instructions() == 6);
    std::uint64_t pairs = 0;
    for (std::size_t first = 0; first < kOpCodeCount; ++first) {
        for (std::size_t second = 0; second < kOpCodeCount; ++second) {
            pairs += counters.pairs[first * kOpCodeCount + second];
        }
    }
    CHECK(pairs == 5);
    CHECK(counters.pairs[static_cast<std::size_t>(OpCode::Const) * kOpCodeCount +
                         static_cast<std::size_t>(OpCode::Const)] == 1);
    CHECK(contains(engine.pair_report(), "CONST"));
    out.flush();
    std::fclose(file);
}

TEST_CASE("pair counters: a second run starts a new sequence") {
    // call_global after run(): the first instruction of the call does not follow the last
    // instruction of the script.
    std::unique_ptr<Program> program = resolved("fn f() { return 1; }\n");
    std::FILE* file = std::tmpfile();
    Heap heap;
    Output out(file);
    RegisterEngine engine(heap, out);
    REQUIRE(engine.run(*program).ok());
    std::uint64_t before = engine.counters().instructions();
    REQUIRE(engine.call_global("f").ok());
    std::uint64_t pairs = 0;
    for (std::uint64_t n : engine.counters().pairs) pairs += n;
    // Only pairs inside one run are counted, so there is one fewer pair than dispatches per run
    // (the start row, the last row of `pairs`, is excluded from this sum by being the extra row
    // that is also counted: add it back).
    std::uint64_t start_row = 0;
    for (std::size_t op = 0; op < kRegOpCount; ++op) {
        start_row += engine.counters().pairs[kRegOpCount * kRegOpCount + op];
    }
    CHECK(start_row == 2);  // one first instruction per run
    CHECK(pairs - start_row == engine.counters().instructions() - 2);
    CHECK(engine.counters().instructions() > before);
    out.flush();
    std::fclose(file);
}
#endif
