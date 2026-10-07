#include <doctest.h>

#include <cstdio>
#include <memory>
#include <string>

#include "engine.h"
#include "output.h"
#include "parser.h"
#include "resolver.h"
#include "runtime/ops.h"
#include "vm_reg.h"
#include "vm_stack.h"

using namespace rung;

namespace {

struct RegRun {
    std::string output;
    EngineResult result;
    std::string stats;
};

std::string slurp_file(std::FILE* file) {
    std::string text;
    std::rewind(file);
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, n);
    return text;
}

std::unique_ptr<Program> compile_program(const std::string& source) {
    ParseResult parsed = parse(source);
    REQUIRE_MESSAGE(parsed.ok(), parsed.error->message);
    auto error = resolve(*parsed.program);
    REQUIRE_MESSAGE(!error.has_value(), error->message);
    return std::move(parsed.program);
}

// Runs `source` on the engine called `engine_name`. The register VM does not recurse on the C++
// stack, so no big thread stack is needed.
RegRun run_on(const std::string& engine_name, const std::string& source, bool stress = false) {
    std::unique_ptr<Program> program = compile_program(source);
    std::FILE* file = std::tmpfile();
    RegRun run;
    {
        Heap heap;
        heap.set_stress(stress);
        Output out(file);
        std::unique_ptr<Engine> engine = make_engine(engine_name, heap, out);
        REQUIRE(engine != nullptr);
        run.result = engine->run(*program);
        out.flush();
        run.stats = engine->stats_report();
    }
    run.output = slurp_file(file);
    std::fclose(file);
    return run;
}

RegRun run_reg(const std::string& source, bool stress = false) {
    return run_on("register", source, stress);
}

}  // namespace

TEST_CASE("register engine: prints, arithmetic and control flow") {
    RegRun run = run_reg(R"(
        let i = 0;
        while (i < 3) { print i * 2 + 1; i = i + 1; }
        print "a" + "b";
        print 7 / 2;
        print 7.0 / 2;
        print nil or "fallback";
        print 1 and 2;
        print !nil;
        print -(3);
        print [1, 2.5, "x"];
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "1\n3\n5\nab\n3\n3.5\nfallback\n2\ntrue\n-3\n[1, 2.5, x]\n");
}

TEST_CASE("register engine: locals live in registers across blocks and loops") {
    RegRun run = run_reg(R"(
        {
          let total = 0;
          for (let i = 0; i < 5; i = i + 1) { let sq = i * i; total = total + sq; }
          print total;
          let flag = total > 10 and total < 100;
          print flag;
        }
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "30\ntrue\n");
}

TEST_CASE("register engine: an instruction reads its operands before writing its result") {
    // Each of these names one register as both a source and the destination, or assigns a
    // variable that an earlier operand already read (notes D14). The stack VM and the
    // tree-walker evaluate the left operand first; the register VM must agree.
    RegRun run = run_reg(R"(
        {
          let a = 1;
          print a + (a = 5);
          print a;
          a = a + 1;
          print a;
          let b = 2;
          a = (b + 1) and a;
          print a;
          let c = 10;
          fn bump() { c = c + 1; return 0; }
          print c + bump();
          print c;
          let xs = [1, 2, 3];
          xs[0] = xs[0] + xs[2];
          print xs;
        }
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "6\n5\n6\n6\n10\n11\n[4, 2, 3]\n");
}

TEST_CASE("register engine: a call's arguments become the callee's first registers") {
    RegRun run = run_reg(R"(
        fn add3(a, b, c) { let s = a + b; return s + c; }
        fn twice(f, x) { return f(f(x, 1, 1), 1, 1); }
        print add3(1, 2, 3);
        print twice(add3, 10);
        {
          let local = 7;
          print add3(local, local, add3(1, 1, 1));
          print local;
        }
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "6\n14\n17\n7\n");
}

TEST_CASE("register engine: 255 arguments in one call") {
    std::string params;
    std::string args;
    for (int i = 0; i < 255; ++i) {
        params += (i == 0 ? "p" : ", p") + std::to_string(i);
        args += (i == 0 ? "" : ", ") + std::to_string(i);
    }
    RegRun run = run_reg("fn f(" + params + ") { return p0 + p127 + p254; }\nprint f(" + args +
                         ");\n");
    CHECK(run.result.ok());
    CHECK(run.output == "381\n");
}

TEST_CASE("register engine: a counter closure outlives the function that made it") {
    // `n` lives in a register of make_counter's window while it runs (an open upvalue), and is
    // copied into the upvalue object when make_counter returns (closed).
    RegRun run = run_reg(R"(
        fn make_counter() {
          let n = 0;
          fn next() { n = n + 1; return n; }
          fn peek() { return n; }
          return [next, peek];
        }
        let a = make_counter();
        let b = make_counter();
        let a_next = a[0]; let a_peek = a[1]; let b_next = b[0];
        print a_next(); print a_next(); print b_next(); print a_next(); print a_peek();
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "1\n2\n1\n3\n3\n");
}

TEST_CASE("register engine: a block's captured local is closed when the block ends") {
    // The CLOSE at the end of the first block must copy x out before the second block reuses
    // its register for y.
    RegRun run = run_reg(R"(
        let f;
        { let x = "inside"; fn show() { print x; } f = show; x = "changed"; }
        { let y = "reused"; f(); print y; }
        f();
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "changed\nreused\nchanged\n");
}

TEST_CASE("register engine: a local function calls itself through its own register") {
    RegRun run = run_reg(R"(
        {
          fn fact(n) { if (n < 2) return 1; return n * fact(n - 1); }
          print fact(10);
        }
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "3628800\n");
}

TEST_CASE("register engine: scoping follows the resolver, not the run-time order (notes D11)") {
    RegRun run = run_reg(R"(
        let a = "global";
        { fn show() { print a; } show(); let a = "block"; show(); }
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "global\nglobal\n");
}

TEST_CASE("register engine: array literals longer than one batch") {
    std::string elements;
    for (int i = 0; i < 120; ++i) elements += (i == 0 ? "" : ", ") + std::to_string(i);
    RegRun run = run_reg("{ let xs = [" + elements +
                         "]; print len(xs); print xs[0] + xs[50] + xs[119]; }\n");
    CHECK(run.result.ok());
    CHECK(run.output == "120\n169\n");
}

TEST_CASE("register engine: runtime errors carry the line of the failing instruction") {
    RegRun run = run_reg("print 1;\nprint 2\n  + nil;\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 3);
    CHECK(run.result.runtime_error->message == "operands must be two numbers or two strings");
    CHECK(run.output == "1\n");

    run = run_reg("fn f() { return g; }\n\nf();\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 1);
    CHECK(run.result.runtime_error->message == "undefined variable 'g'");

    run = run_reg("let a = 1;\na(\n);\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 2);
    CHECK(run.result.runtime_error->message == kErrNotCallable);

    run = run_reg("fn f(a) {}\n\nf();\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 3);
    CHECK(run.result.runtime_error->message == "expected 1 arguments but got 0");

    run = run_reg("let xs = [1];\nxs[\n1] = 2;\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 2);
    CHECK(run.result.runtime_error->message == "array index out of range");
}

TEST_CASE("register engine: the call depth limit is exactly 10,000") {
    RegRun run = run_reg(
        "fn down(n) { if (n == 0) return 0; return 1 + down(n - 1); }\n"
        "print down(9999);\n"
        "print down(9999);\n"  // the frames came back off after the first run
        "print down(10000);\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->message == kErrStackOverflow);
    CHECK(run.result.runtime_error->line == 1);
    CHECK(run.output == "9999\n9999\n");
}

TEST_CASE("register engine: deep recursion with wide windows still reaches the depth limit") {
    // Every level holds 200 locals and calls itself above them, so each window starts about 200
    // registers above its caller's: 10,000 levels need about two million registers, which the
    // register file holds (see kRegisterSlots).
    std::string locals;
    for (int i = 0; i < 200; ++i) locals += "let v" + std::to_string(i) + " = n; ";
    RegRun run = run_reg("fn deep(n) { " + locals +
                         "if (n == 0) return v199; return deep(n - 1) + 1; }\n"
                         "print deep(9999);\n"
                         "print deep(10000);\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->message == kErrStackOverflow);
    CHECK(run.output == "9999\n");
}

TEST_CASE("register engine: natives, and errors from natives, report the call line") {
    RegRun run = run_reg("print len([1, 2, 3]) + len(\"ab\");\nprint array(2, 0);\n");
    CHECK(run.result.ok());
    CHECK(run.output == "5\n[0, 0]\n");

    run = run_reg("print 1;\nlen(\n  5);\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 2);
    CHECK(run.result.runtime_error->message == "len expects an array or a string");
}

TEST_CASE("register engine: call_global runs a zero-argument function and returns its value") {
    std::unique_ptr<Program> program = compile_program(
        "let calls = 0;\n"
        "fn run() { calls = calls + 1; return calls * 10; }\n"
        "fn bad() { return 1 + nil; }\n"
        "fn needs_one(a) { return a; }\n"
        "let f = 5;\n");
    std::FILE* file = std::tmpfile();
    Heap heap;
    Output out(file);
    RegisterEngine engine(heap, out);
    REQUIRE(engine.run(*program).ok());

    CallResult first = engine.call_global("run");
    REQUIRE(first.ok());
    CHECK(as_int(first.value) == 10);
    CallResult second = engine.call_global("run");
    REQUIRE(second.ok());
    CHECK(as_int(second.value) == 20);

    CallResult missing = engine.call_global("nope");
    REQUIRE(missing.runtime_error.has_value());
    CHECK(missing.runtime_error->message == "undefined variable 'nope'");
    CallResult not_callable = engine.call_global("f");
    REQUIRE(not_callable.runtime_error.has_value());
    CHECK(not_callable.runtime_error->message == "can only call functions");
    CHECK(not_callable.runtime_error->line == 0);
    CallResult wrong_arity = engine.call_global("needs_one");
    REQUIRE(wrong_arity.runtime_error.has_value());
    CHECK(wrong_arity.runtime_error->message == "expected 1 arguments but got 0");
    CallResult failing = engine.call_global("bad");
    REQUIRE(failing.runtime_error.has_value());
    CHECK(failing.runtime_error->line == 3);
    CHECK(engine.call_global("run").ok());  // the engine is still usable after an error
    CallResult native = engine.call_global("clock");
    REQUIRE(native.ok());
    CHECK(is_float(native.value));
    std::fclose(file);
}

TEST_CASE("register engine: --gc-stress changes nothing a program can see") {
    const char* source = R"(
        fn make(n) { let xs = array(n, "x"); fn get(i) { return xs[i] + "!"; } return get; }
        let getters = [make(2), make(3), make(4)];
        let i = 0;
        while (i < 3) { print getters[i](i) + "-" + "ok"; i = i + 1; }
        print getters[0](5);
    )";
    RegRun normal = run_reg(source);
    RegRun stressed = run_reg(source, true);
    REQUIRE(normal.result.runtime_error.has_value());
    REQUIRE(stressed.result.runtime_error.has_value());
    CHECK(normal.result.runtime_error->message == "array index out of range");
    CHECK(stressed.result.runtime_error->message == normal.result.runtime_error->message);
    CHECK(stressed.output == "x!-ok\nx!-ok\nx!-ok\n");
    CHECK(stressed.output == normal.output);
}

TEST_CASE("register engine: registers a returned call left behind are never marked") {
    // `wide` fills a big window with arrays and returns. Its registers are above the script's
    // window, so the next collections free those arrays while the registers still point at
    // them. `wide_again` then gets a window over the same registers: the call sets them to nil
    // before anything can collect, so the collector never follows a freed pointer (which
    // AddressSanitizer would report in the asan presets).
    RegRun run = run_reg(R"(
        fn wide() { let a = [1]; let b = [2]; let c = [3]; let d = [4]; return 0; }
        fn wide_again() { let s = "x" + "y"; let t = s + "z"; let u = [s, t]; return u; }
        wide();
        let i = 0;
        while (i < 5) { let junk = "j" + "unk"; i = i + 1; }
        print wide_again();
    )", true);
    CHECK(run.result.ok());
    CHECK(run.output == "[xy, xyz]\n");
}

TEST_CASE("register engine: closures, strings and arrays survive collecting on every allocation") {
    RegRun run = run_reg(R"(
        fn counter() { let n = 0; fn next() { n = n + 1; return n; } return next; }
        let c = counter();
        let i = 0;
        let names = array(3, nil);
        while (i < 3) { names[i] = "n" + "um"; c(); i = i + 1; }
        print c();
        print names;
    )", true);
    CHECK(run.result.ok());
    CHECK(run.output == "4\n[num, num, num]\n");
}

TEST_CASE("register engine: closures and functions are garbage collected") {
    // The same loop body run 10 and 1000 times must leave the same number of live objects.
    auto live_after = [](int iterations) {
        std::unique_ptr<Program> program =
            compile_program("let i = 0;\nwhile (i < " + std::to_string(iterations) +
                            ") { let xs = [i]; fn f() { return xs; } i = i + 1; }\n");
        std::FILE* file = std::tmpfile();
        Heap heap;
        Output out(file);
        RegisterEngine engine(heap, out);
        REQUIRE(engine.run(*program).ok());
        heap.collect();
        std::size_t live = heap.stats().live_objects;
        std::fclose(file);
        return live;
    };
    CHECK(live_after(10) == live_after(1000));
}

// Deep recursion is left out on purpose: the tree-walker needs the CLI's 512 MiB thread for it,
// and the conformance suite already compares every engine on it.
TEST_CASE("register engine: agrees with the tree-walker and the stack VM on the same programs") {
    const char* programs[] = {
        "print 2147483647 + 1; print -7 % 3; print 7 / 2; print 1 == 1.0; print 0.1 + 0.2;",
        "fn fib(n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); } print fib(15);",
        "let a = [1, 2, 3]; a[1] = a[0] + a[2]; print a; print len(a);",
        "for (let i = 0; i < 3; i = i + 1) { fn f() { return i; } print f(); }",
        "{ let a = 1; let b = a + (a = 2) * a; print a; print b; }",
        "{ let a = [0]; a[0] = (a = [5])[0] + 1; print a; }",
        "print \"x\" + 1;",
        "print [1] < [2];",
        "let a = array(2, 0); a[2] = 1;",
        "print 1 / 0;",
        "print -nil;",
    };
    for (const char* source : programs) {
        CAPTURE(source);
        RegRun tree = run_on("tree", source);
        RegRun stack = run_on("stack", source);
        RegRun reg = run_on("register", source);
        CHECK(reg.output == tree.output);
        CHECK(reg.output == stack.output);
        CHECK(reg.result.runtime_error.has_value() == tree.result.runtime_error.has_value());
        if (reg.result.runtime_error && tree.result.runtime_error) {
            CHECK(reg.result.runtime_error->line == tree.result.runtime_error->line);
            CHECK(reg.result.runtime_error->message == tree.result.runtime_error->message);
        }
    }
}

TEST_CASE("make_engine knows the register engine") {
    Heap heap;
    std::FILE* file = std::tmpfile();
    Output out(file);
    std::unique_ptr<Engine> engine = make_engine("register", heap, out);
    REQUIRE(engine != nullptr);
    CHECK(engine->name() == "register");
    bool listed = false;
    for (std::string_view name : engine_names()) listed = listed || name == "register";
    CHECK(listed);
    std::fclose(file);
}

#if RUNG_VM_COUNTERS
TEST_CASE("register engine: counters count instructions per opcode and calls") {
    std::unique_ptr<Program> program = compile_program("print 1 + 2;\n");
    std::FILE* file = std::tmpfile();
    Heap heap;
    Output out(file);
    RegisterEngine engine(heap, out);
    REQUIRE(engine.run(*program).ok());
    // ADD r0 k k, PRINT r0, RETURN_NIL: where the stack VM dispatches six (CONST CONST ADD
    // PRINT NIL RETURN), because both operands are constants named in the ADD itself.
    const RegVmCounters& counters = engine.counters();
    CHECK(counters.instructions() == 3);
    CHECK(counters.by_opcode[static_cast<std::size_t>(RegOp::Add)] == 1);
    CHECK(counters.by_opcode[static_cast<std::size_t>(RegOp::Print)] == 1);
    CHECK(counters.rung_calls == 0);  // the script is not a call
    CHECK(engine.stats_report().find("3 instructions dispatched") != std::string::npos);
    CHECK(engine.stats_report().find("ADD") != std::string::npos);
    out.flush();  // `out` still holds the program's "3": it must not flush to a closed file
    std::fclose(file);

    program = compile_program("fn f() { return 1; }\nf(); f(); len([1]);\n");
    file = std::tmpfile();
    Output out2(file);
    RegisterEngine engine2(heap, out2);
    REQUIRE(engine2.run(*program).ok());
    CHECK(engine2.counters().rung_calls == 2);
    CHECK(engine2.counters().native_calls == 1);
    std::fclose(file);
}

TEST_CASE("register engine: dispatches fewer instructions than the stack VM") {
    // The point of the rung (spec §5.5): the same work in fewer, bigger instructions. Exact
    // counts are in docs/notes.md §5; here only the direction is pinned, plus the calls, which
    // must be identical.
    std::unique_ptr<Program> program = compile_program(
        "fn fib(n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }\n"
        "let i = 0; let total = 0;\n"
        "while (i < 100) { total = total + i * 2; i = i + 1; }\n"
        "print fib(15) + total;\n");
    std::FILE* file = std::tmpfile();
    Heap heap;
    Output out(file);
    StackEngine stack(heap, out);
    REQUIRE(stack.run(*program).ok());
    RegisterEngine reg(heap, out);
    REQUIRE(reg.run(*program).ok());
    out.flush();
    CHECK(slurp_file(file) == "10510\n10510\n");
    CHECK(reg.counters().rung_calls == stack.counters().rung_calls);
    CHECK(reg.counters().instructions() < stack.counters().instructions());
    std::fclose(file);
}
#else
TEST_CASE("register engine: stats say the counters are compiled out") {
    RegRun run = run_reg("print 1;\n");
    CHECK(run.stats.find("not compiled in") != std::string::npos);
}
#endif
