#include "bench.h"

#include <chrono>
#include <cstdio>

#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

#include "runtime/ops.h"

namespace rung {

BenchResult run_bench(Engine& engine, std::size_t iterations) {
    BenchResult bench;
    bench.iterations_ns.reserve(iterations);
    for (std::size_t i = 0; i < iterations; ++i) {
        auto start = std::chrono::steady_clock::now();
        CallResult call = engine.call_global("run");
        auto stop = std::chrono::steady_clock::now();
        if (!call.ok()) {
            bench.compile_error = call.compile_error;
            bench.runtime_error = call.runtime_error;
            return bench;
        }
        bench.iterations_ns.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
        // The returned value is not rooted (see CallResult), so print it before anything can
        // allocate. Done after the clock stops, and only once: only the last result is kept.
        if (i + 1 == iterations) {
            bench.result.clear();
            print_value(call.value, bench.result);
        }
    }
    return bench;
}

void set_benchmark_thread_qos() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

std::string json_quote(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c));
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

std::string bench_json(std::string_view engine_name, const BenchResult& bench,
                       const HeapStats& heap_stats) {
    std::string out = "{\"engine\": " + json_quote(engine_name) + ", \"iterations_ns\": [";
    const char* separator = "";
    for (long long ns : bench.iterations_ns) {
        out += separator;
        out += std::to_string(ns);
        separator = ", ";
    }
    out += "], \"result\": " + json_quote(bench.result);
    out += ", \"heap\": {\"objects_allocated\": " + std::to_string(heap_stats.objects_allocated);
    out += ", \"bytes_allocated\": " + std::to_string(heap_stats.bytes_allocated);
    out += ", \"collections\": " + std::to_string(heap_stats.collections);
    out += ", \"peak_live_bytes\": " + std::to_string(heap_stats.peak_live_bytes) + "}}\n";
    return out;
}

}  // namespace rung
