#include <doctest.h>

#include <cstdio>
#include <memory>
#include <string>

#include "engine.h"
#include "engine_tree.h"
#include "output.h"
#include "parser.h"
#include "resolver.h"
#include "runtime/ops.h"
#include "vm_stack.h"

using namespace rung;

namespace {

struct StackRun {
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

// Runs `source` on the engine called `engine_name`. Unlike the tree-walker, the stack VM does
// not recurse on the C++ stack, so no big thread stack is needed.
StackRun run_on(const std::string& engine_name, const std::string& source, bool stress = false) {
    std::unique_ptr<Program> program = compile_program(source);
    std::FILE* file = std::tmpfile();
    StackRun run;
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

StackRun run_stack(const std::string& source, bool stress = false) {
    return run_on("stack", source, stress);
}

}  // namespace

TEST_CASE("stack engine: prints, arithmetic and control flow") {
    StackRun run = run_stack(R"(
        let i = 0;
        while (i < 3) { print i * 2 + 1; i = i + 1; }
        print "a" + "b";
        print 7 / 2;
        print 7.0 / 2;
        print nil or "fallback";
        print 1 and 2;
        print [1, 2.5, "x"];
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "1\n3\n5\nab\n3\n3.5\nfallback\n2\n[1, 2.5, x]\n");
}

TEST_CASE("stack engine: a counter closure outlives the function that made it") {
    // `n` lives in make_counter's frame while it runs (an open upvalue), and is copied into the
    // upvalue object when make_counter returns (closed). Each call of make_counter makes a new
    // variable, and two closures over one variable share it.
    StackRun run = run_stack(R"(
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

TEST_CASE("stack engine: a block's captured local is closed when the block ends") {
    StackRun run = run_stack(R"(
        let f;
        { let x = "inside"; fn show() { print x; } f = show; x = "changed"; }
        f();
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "changed\n");
}

TEST_CASE("stack engine: scoping follows the resolver, not the run-time order (notes D11)") {
    StackRun run = run_stack(R"(
        let a = "global";
        { fn show() { print a; } show(); let a = "block"; show(); }
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "global\nglobal\n");
}

TEST_CASE("stack engine: runtime errors carry the line of the failing instruction") {
    StackRun run = run_stack("print 1;\nprint 2\n  + nil;\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 3);
    CHECK(run.result.runtime_error->message == "operands must be two numbers or two strings");
    CHECK(run.output == "1\n");

    run = run_stack("fn f() { return g; }\n\nf();\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 1);
    CHECK(run.result.runtime_error->message == "undefined variable 'g'");

    run = run_stack("let a = 1;\na(\n);\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 2);
    CHECK(run.result.runtime_error->message == kErrNotCallable);

    run = run_stack("fn f(a) {}\n\nf();\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 3);
    CHECK(run.result.runtime_error->message == "expected 1 arguments but got 0");
}

TEST_CASE("stack engine: the call depth limit is exactly 10,000") {
    StackRun run = run_stack(
        "fn down(n) { if (n == 0) return 0; return 1 + down(n - 1); }\n"
        "print down(9999);\n"
        "print down(9999);\n"  // the frames came back off after the first run
        "print down(10000);\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->message == kErrStackOverflow);
    CHECK(run.result.runtime_error->line == 1);
    CHECK(run.output == "9999\n9999\n");
}

TEST_CASE("stack engine: natives, and errors from natives, report the call line") {
    StackRun run = run_stack("print len([1, 2, 3]) + len(\"ab\");\nprint array(2, 0);\n");
    CHECK(run.result.ok());
    CHECK(run.output == "5\n[0, 0]\n");

    run = run_stack("print 1;\nlen(\n  5);\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 2);
    CHECK(run.result.runtime_error->message == "len expects an array or a string");
}

TEST_CASE("stack engine: call_global runs a zero-argument function and returns its value") {
    std::unique_ptr<Program> program = compile_program(
        "let calls = 0;\n"
        "fn run() { calls = calls + 1; return calls * 10; }\n"
        "fn bad() { return 1 + nil; }\n"
        "fn needs_one(a) { return a; }\n"
        "let f = 5;\n");
    std::FILE* file = std::tmpfile();
    Heap heap;
    Output out(file);
    StackEngine engine(heap, out);
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

TEST_CASE("stack engine: --gc-stress changes nothing a program can see") {
    const char* source = R"(
        fn make(n) { let xs = array(n, "x"); fn get(i) { return xs[i] + "!"; } return get; }
        let getters = [make(2), make(3), make(4)];
        let i = 0;
        while (i < 3) { print getters[i](i) + "-" + "ok"; i = i + 1; }
        print getters[0](5);
    )";
    StackRun normal = run_stack(source);
    StackRun stressed = run_stack(source, true);
    REQUIRE(normal.result.runtime_error.has_value());
    REQUIRE(stressed.result.runtime_error.has_value());
    CHECK(normal.result.runtime_error->message == "array index out of range");
    CHECK(stressed.result.runtime_error->message == normal.result.runtime_error->message);
    CHECK(stressed.output == "x!-ok\nx!-ok\nx!-ok\n");
    CHECK(stressed.output == normal.output);
}

TEST_CASE("stack engine: closures, strings and arrays survive a collection on every allocation") {
    StackRun run = run_stack(R"(
        fn counter() { let n = 0; fn next() { n = n + 1; return n; } return next; }
        let c = counter();
        let items = [];
        let i = 0;
        let names = array(3, nil);
        while (i < 3) { names[i] = "n" + "um"; c(); i = i + 1; }
        print c();
        print names;
    )", true);
    CHECK(run.result.ok());
    CHECK(run.output == "4\n[num, num, num]\n");
}

TEST_CASE("stack engine: closures and functions are garbage collected") {
    // The same loop body run 10 and 1000 times must leave the same number of live objects.
    auto live_after = [](int iterations) {
        std::unique_ptr<Program> program =
            compile_program("let i = 0;\nwhile (i < " + std::to_string(iterations) +
                            ") { let xs = [i]; fn f() { return xs; } i = i + 1; }\n");
        std::FILE* file = std::tmpfile();
        Heap heap;
        Output out(file);
        StackEngine engine(heap, out);
        REQUIRE(engine.run(*program).ok());
        heap.collect();
        std::size_t live = heap.stats().live_objects;
        std::fclose(file);
        return live;
    };
    CHECK(live_after(10) == live_after(1000));
}

// Deep recursion is left out on purpose: the tree-walker needs the CLI's 512 MiB thread for it,
// and the conformance suite already compares both engines on it.
TEST_CASE("stack engine: agrees with the tree-walker on the same programs") {
    const char* programs[] = {
        "print 2147483647 + 1; print -7 % 3; print 7 / 2; print 1 == 1.0; print 0.1 + 0.2;",
        "fn fib(n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); } print fib(15);",
        "let a = [1, 2, 3]; a[1] = a[0] + a[2]; print a; print len(a);",
        "for (let i = 0; i < 3; i = i + 1) { fn f() { return i; } print f(); }",
        "print \"x\" + 1;",
        "print [1] < [2];",
        "let a = array(2, 0); a[2] = 1;",
        "print 1 / 0;",
    };
    for (const char* source : programs) {
        StackRun tree = run_on("tree", source);
        StackRun stack = run_on("stack", source);
        CHECK(stack.output == tree.output);
        CHECK(stack.result.runtime_error.has_value() == tree.result.runtime_error.has_value());
        if (stack.result.runtime_error && tree.result.runtime_error) {
            CHECK(stack.result.runtime_error->line == tree.result.runtime_error->line);
            CHECK(stack.result.runtime_error->message == tree.result.runtime_error->message);
        }
    }
}

TEST_CASE("make_engine knows the stack engine") {
    Heap heap;
    std::FILE* file = std::tmpfile();
    Output out(file);
    CHECK(make_engine("stack", heap, out) != nullptr);
    bool listed = false;
    for (std::string_view name : engine_names()) listed = listed || name == "stack";
    CHECK(listed);
    std::fclose(file);
}

#if RUNG_VM_COUNTERS
TEST_CASE("stack engine: counters count instructions per opcode and calls") {
    std::unique_ptr<Program> program = compile_program("print 1 + 2;\n");
    std::FILE* file = std::tmpfile();
    Heap heap;
    Output out(file);
    StackEngine engine(heap, out);
    REQUIRE(engine.run(*program).ok());
    // CONST CONST ADD PRINT NIL RETURN: the script's own code and its implicit `return nil`.
    const VmCounters& counters = engine.counters();
    CHECK(counters.instructions() == 6);
    CHECK(counters.by_opcode[static_cast<std::size_t>(OpCode::Const)] == 2);
    CHECK(counters.by_opcode[static_cast<std::size_t>(OpCode::Add)] == 1);
    CHECK(counters.rung_calls == 0);  // the script is not a call
    CHECK(engine.stats_report().find("6 instructions dispatched") != std::string::npos);
    out.flush();  // `out` still holds the program's "3": it must not flush to a closed file
    std::fclose(file);

    program = compile_program("fn f() { return 1; }\nf(); f(); len([1]);\n");
    file = std::tmpfile();
    Output out2(file);
    StackEngine engine2(heap, out2);
    REQUIRE(engine2.run(*program).ok());
    CHECK(engine2.counters().rung_calls == 2);
    CHECK(engine2.counters().native_calls == 1);
    std::fclose(file);
}
#else
TEST_CASE("stack engine: stats say the counters are compiled out") {
    StackRun run = run_stack("print 1;\n");
    CHECK(run.stats.find("not compiled in") != std::string::npos);
}
#endif
