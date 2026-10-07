#include <doctest.h>

#include <cstdio>
#include <memory>
#include <string>

#include "bench.h"
#include "engine_tree.h"
#include "output.h"
#include "parser.h"
#include "resolver.h"

using namespace rung;

namespace {

std::unique_ptr<Program> compile(const std::string& source) {
    ParseResult parsed = parse(source);
    REQUIRE_MESSAGE(parsed.ok(), parsed.error->message);
    auto error = resolve(*parsed.program);
    REQUIRE_MESSAGE(!error.has_value(), error->message);
    return std::move(parsed.program);
}

}  // namespace

TEST_CASE("bench: times each call to run and keeps the last result") {
    // Every call bumps a global counter, so the result proves which call was the last one.
    std::unique_ptr<Program> program = compile("let n = 0; fn run() { n = n + 1; return n * 2; }");
    Heap heap;
    Output out(nullptr);  // the program prints nothing, so this is never written to
    TreeEngine engine(heap, out);
    REQUIRE(engine.run(*program).ok());

    BenchResult bench = run_bench(engine, 5);
    CHECK(bench.ok());
    CHECK(bench.iterations_ns.size() == 5);
    for (long long ns : bench.iterations_ns) CHECK(ns >= 0);
    CHECK(bench.result == "10");
}

TEST_CASE("bench: the result is printed like print would") {
    std::unique_ptr<Program> program = compile("fn run() { return 0.5 + 1; }");
    Heap heap;
    Output out(nullptr);
    TreeEngine engine(heap, out);
    REQUIRE(engine.run(*program).ok());
    CHECK(run_bench(engine, 1).result == "1.5");
}

TEST_CASE("bench: a runtime error stops the run and is reported") {
    std::unique_ptr<Program> program = compile("fn run() { return 1 / 0; }");
    Heap heap;
    Output out(nullptr);
    TreeEngine engine(heap, out);
    REQUIRE(engine.run(*program).ok());

    BenchResult bench = run_bench(engine, 3);
    REQUIRE(bench.runtime_error.has_value());
    CHECK(bench.runtime_error->message == "division by zero");
    CHECK(bench.iterations_ns.empty());
}

TEST_CASE("bench: a missing run function is an error, not a crash") {
    std::unique_ptr<Program> program = compile("let x = 1;");
    Heap heap;
    Output out(nullptr);
    TreeEngine engine(heap, out);
    REQUIRE(engine.run(*program).ok());
    CHECK(run_bench(engine, 1).runtime_error.has_value());
}

TEST_CASE("bench: json has the engine, times, result and heap statistics") {
    BenchResult bench;
    bench.iterations_ns = {120, 95};
    bench.result = "42";
    HeapStats stats;
    stats.objects_allocated = 7;
    stats.bytes_allocated = 512;
    stats.collections = 1;
    stats.peak_live_bytes = 300;
    CHECK(bench_json("tree", bench, stats) ==
          "{\"engine\": \"tree\", \"iterations_ns\": [120, 95], \"result\": \"42\", "
          "\"heap\": {\"objects_allocated\": 7, \"bytes_allocated\": 512, \"collections\": 1, "
          "\"peak_live_bytes\": 300}}\n");
}

TEST_CASE("bench: json strings are escaped") {
    CHECK(json_quote("plain") == "\"plain\"");
    CHECK(json_quote("a\"b\\c\nd\te") == "\"a\\\"b\\\\c\\nd\\te\"");
    CHECK(json_quote(std::string("\x01", 1)) == "\"\\u0001\"");
}
