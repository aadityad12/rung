#include <doctest.h>

#include <pthread.h>

#include <cstdio>
#include <memory>
#include <string>

#include "engine.h"
#include "engine_tree.h"
#include "output.h"
#include "parser.h"
#include "resolver.h"
#include "runtime/ops.h"

using namespace rung;

namespace {

// Everything one run produced: what the program printed, and how it ended.
struct Run {
    std::string output;
    EngineResult result;
};

// Reads back what an Output wrote into a temporary file.
std::string slurp(std::FILE* file) {
    std::string text;
    std::rewind(file);
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, n);
    return text;
}

std::unique_ptr<Program> compile(const std::string& source) {
    ParseResult parsed = parse(source);
    REQUIRE_MESSAGE(parsed.ok(), parsed.error->message);
    auto error = resolve(*parsed.program);
    REQUIRE_MESSAGE(!error.has_value(), error->message);
    return std::move(parsed.program);
}

// Runs `body` on a thread with a 512 MiB stack, like the CLI does (notes D12): the tree-walker
// recurses on the C++ stack, and the test thread's default stack is far too small for 10,000
// nested calls. Nothing doctest-related runs on the helper thread.
template <class Body>
void on_big_stack(Body body) {
    struct Job {
        Body* body;
    } job{&body};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, std::size_t{512} * 1024 * 1024);
    pthread_t thread;
    int created = pthread_create(
        &thread, &attr,
        [](void* arg) -> void* {
            (*static_cast<Job*>(arg)->body)();
            return nullptr;
        },
        &job);
    pthread_attr_destroy(&attr);
    REQUIRE(created == 0);
    pthread_join(thread, nullptr);
}

Run run_source(const std::string& source, bool stress = false) {
    std::unique_ptr<Program> program = compile(source);
    std::FILE* file = std::tmpfile();
    Run run;
    on_big_stack([&] {
        Heap heap;
        heap.set_stress(stress);
        Output out(file);
        TreeEngine engine(heap, out);
        run.result = engine.run(*program);
        out.flush();
    });
    run.output = slurp(file);
    std::fclose(file);
    return run;
}

}  // namespace

TEST_CASE("tree engine: prints, arithmetic and control flow") {
    Run run = run_source(R"(
        let i = 0;
        while (i < 3) { print i * 2 + 1; i = i + 1; }
        print "a" + "b";
        print 7 / 2;
        print 7.0 / 2;
        print nil or "fallback";
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "1\n3\n5\nab\n3\n3.5\nfallback\n");
}

TEST_CASE("tree engine: closures share the variable they captured") {
    Run run = run_source(R"(
        fn make_counter() {
          let n = 0;
          fn next() { n = n + 1; return n; }
          return next;
        }
        let a = make_counter();
        let b = make_counter();
        print a(); print a(); print b(); print a();
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "1\n2\n1\n3\n");
}

TEST_CASE("tree engine: scoping follows the resolver, not the run-time order (notes D11)") {
    Run run = run_source(R"(
        let a = "global";
        { fn show() { print a; } show(); let a = "block"; show(); }
    )");
    CHECK(run.result.ok());
    CHECK(run.output == "global\nglobal\n");
}

TEST_CASE("tree engine: runtime errors carry the line of the failing operator") {
    Run run = run_source("print 1;\nprint 2\n  + nil;\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 3);
    CHECK(run.result.runtime_error->message == "operands must be two numbers or two strings");
    CHECK(run.output == "1\n");  // output before the error is kept

    run = run_source("fn f() { return g; }\n\nf();\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->line == 1);
    CHECK(run.result.runtime_error->message == "undefined variable 'g'");
}

// ThreadSanitizer on macOS cannot use more than about 90 MB of a big thread stack (it faults
// there whatever size was asked for; measured with a plain recursive function), and 10,000
// nested Rung calls need more than that. The other presets run this test.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool kUnderTsan = true;
#else
constexpr bool kUnderTsan = false;
#endif
#else
constexpr bool kUnderTsan = false;
#endif

TEST_CASE("tree engine: the call depth limit is exactly 10,000" * doctest::skip(kUnderTsan)) {
    Run run = run_source(
        "fn down(n) { if (n == 0) return 0; return 1 + down(n - 1); }\n"
        "print down(9999);\n"
        "print down(9999);\n"  // the counter came back down after the first run
        "print down(10000);\n");
    REQUIRE(run.result.runtime_error.has_value());
    CHECK(run.result.runtime_error->message == kErrStackOverflow);
    CHECK(run.result.runtime_error->line == 1);
    CHECK(run.output == "9999\n9999\n");
}

TEST_CASE("tree engine: call_global runs a zero-argument function and returns its value") {
    std::unique_ptr<Program> program = compile(
        "let calls = 0;\n"
        "fn run() { calls = calls + 1; return calls * 10; }\n"
        "fn bad() { return 1 + nil; }\n"
        "let f = 5;\n");
    std::FILE* file = std::tmpfile();
    Heap heap;
    Output out(file);
    TreeEngine engine(heap, out);
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
    CallResult failing = engine.call_global("bad");
    REQUIRE(failing.runtime_error.has_value());
    CHECK(failing.runtime_error->line == 3);
    CHECK(engine.call_global("run").ok());  // the engine is still usable after an error
    std::fclose(file);
}

TEST_CASE("tree engine: --gc-stress changes nothing a program can see") {
    const char* source = R"(
        fn make(n) { let xs = array(n, "x"); fn get(i) { return xs[i] + "!"; } return get; }
        let getters = [make(2), make(3), make(4)];
        let i = 0;
        while (i < 3) { print getters[i](i) + "-" + "ok"; i = i + 1; }
        print getters[0](5);
    )";
    Run normal = run_source(source);
    Run stressed = run_source(source, true);
    REQUIRE(normal.result.runtime_error.has_value());
    REQUIRE(stressed.result.runtime_error.has_value());
    CHECK(normal.result.runtime_error->message == "array index out of range");
    CHECK(stressed.result.runtime_error->message == normal.result.runtime_error->message);
    CHECK(stressed.output == "x!-ok\nx!-ok\nx!-ok\n");
    CHECK(stressed.output == normal.output);
}

TEST_CASE("tree engine: scopes and closures are garbage collected") {
    // The same loop body run 10 and 1000 times must leave the same number of live objects.
    auto live_after = [](int iterations) {
        std::unique_ptr<Program> program =
            compile("let i = 0;\nwhile (i < " + std::to_string(iterations) +
                    ") { let xs = [i]; fn f() { return xs; } i = i + 1; }\n");
        std::FILE* file = std::tmpfile();
        Heap heap;
        Output out(file);
        TreeEngine engine(heap, out);
        REQUIRE(engine.run(*program).ok());
        heap.collect();
        std::size_t live = heap.stats().live_objects;
        std::fclose(file);
        return live;
    };
    CHECK(live_after(10) == live_after(1000));
}

TEST_CASE("Output buffers until flushed") {
    std::FILE* file = std::tmpfile();
    Output out(file);
    out.write("hello ");
    out.write("world\n");
    // The text is still in Output's own buffer, so nothing has reached the file.
    CHECK(slurp(file).empty());
    out.flush();
    CHECK(slurp(file) == "hello world\n");
    std::fclose(file);
}

TEST_CASE("make_engine knows the tree engine and rejects unknown names") {
    Heap heap;
    std::FILE* file = std::tmpfile();
    Output out(file);
    CHECK(make_engine("tree", heap, out) != nullptr);
    CHECK(make_engine("nonsense", heap, out) == nullptr);
    CHECK(engine_names().front() == "tree");
    std::fclose(file);  // `out` has nothing buffered, so its destructor does not touch the file
}
