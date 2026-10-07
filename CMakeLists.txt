cmake_minimum_required(VERSION 3.20)
project(rung LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RUNG_WERROR "Treat compiler warnings as errors" ON)
set(RUNG_SANITIZE "" CACHE STRING "Sanitizers to build with: empty, 'address,undefined', or 'thread'")

# Instruction and call counters in the stack and register VMs (notes D5). Compiled out entirely when OFF, so a
# release build pays nothing. Debug-like presets turn it ON; release leaves it OFF.
option(RUNG_VM_COUNTERS "Count instructions dispatched per opcode and calls in the VMs" OFF)

# Computed-goto dispatch in both VMs (ladder rung 3a, notes D4). OFF keeps the portable
# `switch`. Labels-as-values are a GNU extension that clang supports on every platform we build.
option(RUNG_COMPUTED_GOTO "Dispatch bytecode with computed goto instead of a switch" OFF)

# NaN-boxed values (ladder rung 3b, notes D4, D15). OFF keeps the 16-byte tagged struct. Only
# src/runtime/value.h reads the flag; every other file sees the same free-function interface.
option(RUNG_NANBOX "Pack every Value into 8 bytes with NaN-boxing" OFF)

option(RUNG_FUZZ "Build the libFuzzer front-end target (needs a clang with libFuzzer)" OFF)

if(RUNG_FUZZ AND RUNG_SANITIZE)
  message(FATAL_ERROR "RUNG_FUZZ already builds with address,undefined; leave RUNG_SANITIZE empty")
endif()

if(RUNG_SANITIZE)
  add_compile_options(-fsanitize=${RUNG_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${RUNG_SANITIZE})
endif()

if(RUNG_FUZZ)
  # Apple's clang ships no libFuzzer runtime; fail here with the reason rather than at link time.
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
  check_cxx_source_compiles("
    #include <cstddef>
    #include <cstdint>
    extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }" RUNG_HAS_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)
  if(NOT RUNG_HAS_LIBFUZZER)
    message(FATAL_ERROR
      "RUNG_FUZZ needs a clang with the libFuzzer runtime. Apple clang has none: on macOS use "
      "Homebrew LLVM (-DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++); on Linux install "
      "clang's runtime package (libclang-rt-NN-dev, matching your clang).")
  endif()
  # Every library gets coverage instrumentation (fuzzer-no-link) so libFuzzer can see which
  # branches an input reaches inside the lexer, parser and resolver. Only the fuzz target links
  # libFuzzer's main().
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Warnings for Rung's own code only; third-party headers are included as SYSTEM.
add_library(rung_warnings INTERFACE)
target_compile_options(rung_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                                               -Wno-sign-conversion)
if(RUNG_WERROR)
  target_compile_options(rung_warnings INTERFACE -Werror)
endif()

# Everything except main(), so tests link the same code the CLI runs.
add_library(rung_core STATIC
  src/ast.cpp
  src/ast_dump.cpp
  src/bench.cpp
  src/bytecode/chunk.cpp
  src/bytecode/register_code.cpp
  src/compiler_reg.cpp
  src/compiler_stack.cpp
  src/diagnostic.cpp
  src/disassembler.cpp
  src/engine.cpp
  src/engine_tree.cpp
  src/fold.cpp
  src/jit/arm64_emitter.cpp
  src/jit/jit_compiler.cpp
  src/lexer.cpp
  src/output.cpp
  src/runtime/heap.cpp
  src/runtime/natives.cpp
  src/runtime/object.cpp
  src/parser.cpp
  src/resolver.cpp
  src/runtime/ops.cpp
  src/superinstructions.cpp
  src/token.cpp
  src/vm_reg.cpp
  src/vm_stack.cpp
)
target_include_directories(rung_core PUBLIC src)
if(RUNG_VM_COUNTERS)
  # PUBLIC: every translation unit that includes vm_stack.h must agree on the flag.
  target_compile_definitions(rung_core PUBLIC RUNG_VM_COUNTERS=1)
endif()
if(RUNG_COMPUTED_GOTO)
  # Only the translation units with a dispatch loop use `&&label` and `goto *`, which -Wpedantic
  # reports as -Wgnu-label-as-value. Everything else keeps the full warning set.
  target_compile_definitions(rung_core PUBLIC RUNG_COMPUTED_GOTO=1)
  set_source_files_properties(src/vm_stack.cpp src/vm_reg.cpp PROPERTIES COMPILE_OPTIONS
                              -Wno-gnu-label-as-value)
endif()
if(RUNG_NANBOX)
  # PUBLIC: Value's size and layout must agree in every translation unit, tests included.
  target_compile_definitions(rung_core PUBLIC RUNG_NANBOX=1)
endif()
target_link_libraries(rung_core PRIVATE rung_warnings)

# Executable memory only exists on arm64: an x86-64 CPU cannot run the machine code (notes D6).
set(RUNG_HAS_JIT_MEMORY OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(RUNG_HAS_JIT_MEMORY ON)
  target_sources(rung_core PRIVATE src/jit/exec_memory.cpp)
endif()

# The JIT itself (--engine=jit, notes D16) also needs NaN-boxed values: its type guards test
# NaN-box tag bits (notes D3). Elsewhere --engine=jit is a usage error and its conformance runs
# are reported as skipped (notes D6).
set(RUNG_HAS_JIT OFF)
if(RUNG_HAS_JIT_MEMORY AND RUNG_NANBOX)
  set(RUNG_HAS_JIT ON)
  target_sources(rung_core PRIVATE src/jit/jit.cpp)
  # PUBLIC: vm_reg.h has a JIT member only in this configuration, so every includer must agree.
  target_compile_definitions(rung_core PUBLIC RUNG_JIT=1)
endif()

find_package(Threads REQUIRED)  # the engine runs on its own big-stack thread (notes D12)
add_executable(rung src/main.cpp)
target_link_libraries(rung PRIVATE rung_core rung_warnings Threads::Threads)

# Unit tests (doctest, vendored single header).
enable_testing()
add_executable(rung_unit_tests
  tests/unit/test_main.cpp
  tests/unit/arm64_emitter_test.cpp
  tests/unit/bench_test.cpp
  tests/unit/compiler_reg_test.cpp
  tests/unit/compiler_stack_test.cpp
  tests/unit/engine_tree_test.cpp
  tests/unit/fold_test.cpp
  tests/unit/jit_compiler_test.cpp
  tests/unit/lexer_test.cpp
  tests/unit/runtime_heap_test.cpp
  tests/unit/parser_test.cpp
  tests/unit/resolver_test.cpp
  tests/unit/runtime_ops_test.cpp
  tests/unit/runtime_value_test.cpp
  tests/unit/superinstructions_test.cpp
  tests/unit/vm_reg_test.cpp
  tests/unit/vm_stack_test.cpp
)
target_include_directories(rung_unit_tests SYSTEM PRIVATE third_party/doctest)
if(RUNG_HAS_JIT_MEMORY)
  target_sources(rung_unit_tests PRIVATE tests/unit/exec_memory_test.cpp)
endif()
if(RUNG_HAS_JIT)
  target_sources(rung_unit_tests PRIVATE tests/unit/jit_test.cpp)
endif()
target_link_libraries(rung_unit_tests PRIVATE rung_core rung_warnings Threads::Threads)

add_test(NAME unit COMMAND rung_unit_tests)

# Front-end fuzzer (issue #12). Run it with fuzz/run_fuzz.sh; see README, Testing.
if(RUNG_FUZZ)
  add_executable(rung_fuzz_frontend fuzz/fuzz_frontend.cpp)
  target_link_libraries(rung_fuzz_frontend PRIVATE rung_core rung_warnings)
  target_link_options(rung_fuzz_frontend PRIVATE -fsanitize=fuzzer)

  # `-runs=0` replays each seed input once and exits: a quick "does every known input still pass"
  # check under `ctest --preset fuzz`, without doing any fuzzing. libFuzzer reads a directory
  # recursively when given as an argument, so the conformance tree can be passed as it is.
  set(RUNG_FUZZ_SEEDS ${CMAKE_SOURCE_DIR}/examples)
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/conformance)
    list(APPEND RUNG_FUZZ_SEEDS ${CMAKE_SOURCE_DIR}/tests/conformance)
  endif()
  add_test(NAME fuzz_seed_replay COMMAND rung_fuzz_frontend -runs=0 ${RUNG_FUZZ_SEEDS})
endif()

# Conformance suite (notes D13): one ctest entry per engine, plus a run with the collector
# forced to run on every allocation, which is what finds a value the GC cannot see (notes D10).
# The generous per-test timeout is for --gc-stress under the sanitizers.
#
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Under --gc-stress every allocation is a full collection, so a program that keeps N calls
# active does N collections that each visit about N scopes: the work grows with the square of
# the depth. The six tests that go 6,000 to 10,000 calls deep each took more than 4 seconds under
# ASan on a laptop (25 seconds for the deepest) and did not finish in five minutes on the
# Linux arm64 CI runner, so ASan runs skip them (the runner names each one as SKIPPED). They still run under --gc-stress in
# debug and release, and under ASan without it, and every shallower recursion test runs in all.
# ThreadSanitizer builds, which are slower still, skip the same six under --gc-stress.
set(RUNG_GC_STRESS_SKIPS)
if(RUNG_SANITIZE MATCHES "address|thread")
  set(RUNG_GC_STRESS_SKIPS
    --skip functions/call_depth_resets_after_returning.rg
    --skip functions/infinite_recursion_overflows_the_stack.rg
    --skip functions/mutual_recursion_overflows_the_stack.rg
    --skip functions/nine_thousand_nine_hundred_ninety_nine_nested_calls_succeed.rg
    --skip functions/stack_overflow_keeps_earlier_output.rg
    --skip functions/ten_thousand_and_one_nested_calls_overflow.rg)
endif()

# Most of the suite is not registered in ThreadSanitizer builds: the suite's 10,000-deep
# recursion tests need a native stack of 100 MB or more on the tree-walker, and ThreadSanitizer on
# macOS faults after about 90 MB of any big thread stack, whatever size was requested. The
# tree-walker and the VMs are single-threaded, so ThreadSanitizer has nothing to check in them;
# the tsan builds exist for the JIT's compiler thread (notes D8), whose runs are registered after
# this block in every build.
if(NOT RUNG_SANITIZE STREQUAL "thread")

  # Engines that exist on this platform; later engine issues add theirs here.
  set(RUNG_ENGINES tree stack register)

  foreach(engine ${RUNG_ENGINES})
    add_test(NAME conformance-${engine}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
              --rung $<TARGET_FILE:rung> --engine ${engine} --timeout 120)
    add_test(NAME conformance-${engine}-gc-stress
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
              --rung $<TARGET_FILE:rung> --engine ${engine} --gc-stress --timeout 300
              ${RUNG_GC_STRESS_SKIPS})
    # The same suite with the constant-folding pass on (ladder rung 3f): folding must not change
    # any program's output, error line, or exit code.
    add_test(NAME conformance-${engine}-fold
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
              --rung $<TARGET_FILE:rung> --engine ${engine} --fold --timeout 120)
  endforeach()
  add_test(NAME conformance-register-fold-gc-stress
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
            --rung $<TARGET_FILE:rung> --engine register --fold --gc-stress --timeout 300
            ${RUNG_GC_STRESS_SKIPS})

  # The register VM's inline cache (ladder rung 3e) must not change what any program does, so the
  # whole suite runs once more with it on, and once more with the collector forced on every
  # allocation (the cache holds pointers into the globals table, which the collector scans).
  add_test(NAME conformance-register-inline-cache
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
            --rung $<TARGET_FILE:rung> --engine register --inline-cache --timeout 120)
  add_test(NAME conformance-register-fold-inline-cache
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
            --rung $<TARGET_FILE:rung> --engine register --fold --inline-cache --timeout 120)
  add_test(NAME conformance-register-inline-cache-gc-stress
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
            --rung $<TARGET_FILE:rung> --engine register --inline-cache --gc-stress
            --timeout 300 ${RUNG_GC_STRESS_SKIPS})

  # The register VM's superinstructions (ladder rung 3d) must not change what any program does,
  # including which line an error is reported on when either half of a fused pair fails. The suite
  # runs with the flag alone, with the collector on every allocation, and with every register-VM
  # rung at once (the ladder is cumulative, notes D4, and the rungs share the dispatch loop).
  add_test(NAME conformance-register-superinstructions
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
            --rung $<TARGET_FILE:rung> --engine register --superinstructions --timeout 120)
  add_test(NAME conformance-register-superinstructions-gc-stress
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
            --rung $<TARGET_FILE:rung> --engine register --superinstructions --gc-stress
            --timeout 300 ${RUNG_GC_STRESS_SKIPS})
  add_test(NAME conformance-register-all-rungs
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
            --rung $<TARGET_FILE:rung> --engine register --superinstructions --inline-cache
            --fold --timeout 120)

  # The committed opcode-pair counts (docs/pair_counts*.txt, the evidence behind rung 3d's choice
  # of fusions) must be what this build's VM counters produce. Counts, not timings; needs the
  # counters, and not a sanitizer, whose slowdown would only make it take longer.
  if(RUNG_VM_COUNTERS AND NOT RUNG_SANITIZE)
    add_test(NAME pair-counts
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/pair_counts.py
              --rung $<TARGET_FILE:rung> --check)
    add_test(NAME pair-counts-fused
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/pair_counts.py
              --rung $<TARGET_FILE:rung> --superinstructions --check)
    # The same for docs/instruction_counts.txt, the per-benchmark dispatch counts of both VMs
    # that notes section 5 uses to explain the ladder (issue #13).
    add_test(NAME instruction-counts
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/instruction_counts.py
              --rung $<TARGET_FILE:rung> --check)
  endif()

  add_test(NAME bench-cli
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_bench_cli_check.py
            --rung $<TARGET_FILE:rung>)

  # scripts/bench.py and scripts/ladder.py against a fake rung and fixture results (issue #8),
  # plus the check that the README table matches results/. No timing is measured.
  add_test(NAME bench-scripts
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_bench_scripts.py)

  # Benchmark checksums (issue #7): every benchmark once on every engine. Release only: the
  # full-size benchmarks take seconds when optimised and many minutes under -O0 or a sanitizer.
  # This checks results, never timings (notes D5).
  if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT RUNG_SANITIZE)
    set(RUNG_BENCH_ENGINES ${RUNG_ENGINES})
    if(RUNG_HAS_JIT)
      list(APPEND RUNG_BENCH_ENGINES jit)  # at its default threshold, as bench.py runs it
    endif()
    foreach(engine ${RUNG_BENCH_ENGINES})
      add_test(NAME bench-check-${engine}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_bench_check.py
                --rung $<TARGET_FILE:rung> --engine ${engine})
    endforeach()
    add_test(NAME bench-check-register-inline-cache
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_bench_check.py
              --rung $<TARGET_FILE:rung> --engine register --inline-cache)
    add_test(NAME bench-check-register-superinstructions
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_bench_check.py
              --rung $<TARGET_FILE:rung> --engine register --superinstructions)
    add_test(NAME bench-check-register-all-rungs
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_bench_check.py
              --rung $<TARGET_FILE:rung> --engine register --superinstructions --inline-cache)
    if(RUNG_HAS_JIT)
      add_test(NAME bench-check-jit-all-rungs
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_bench_check.py
                --rung $<TARGET_FILE:rung> --engine jit --superinstructions --inline-cache)
      add_test(NAME bench-check-jit-background-all-rungs
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_bench_check.py
                --rung $<TARGET_FILE:rung> --engine jit --superinstructions --inline-cache
                --jit-background)
      # The README marks the JIT row's cells for benchmarks the JIT compiles nothing in
      # (bench/jit_not_compiled.json); this checks that list against --jit-log (issue #24).
      add_test(NAME bench-jit-coverage
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_jit_coverage_check.py
                --rung $<TARGET_FILE:rung>)
    endif()
  endif()

  # The JIT with a threshold of 1, so every function inside the whitelist is compiled on its
  # first call and the whole suite runs through machine code wherever it can (notes D16). In a
  # build without the JIT the runner checks that rung refuses --engine=jit as unavailable, lists
  # every test as skipped, and exits 77, which ctest reports as "Skipped" (notes D6).
  set(RUNG_JIT_ARGS --rung-arg=--jit-threshold=1)
  if(NOT RUNG_HAS_JIT)
    list(APPEND RUNG_JIT_ARGS --expect-unavailable)
  endif()
  add_test(NAME conformance-jit
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
            --rung $<TARGET_FILE:rung> --engine jit ${RUNG_JIT_ARGS} --timeout 120)
  add_test(NAME conformance-jit-gc-stress
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
            --rung $<TARGET_FILE:rung> --engine jit ${RUNG_JIT_ARGS} --gc-stress --timeout 300
            ${RUNG_GC_STRESS_SKIPS})
  add_test(NAME conformance-jit-fold
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
            --rung $<TARGET_FILE:rung> --engine jit ${RUNG_JIT_ARGS} --fold --timeout 120)
  # The JIT is the register VM plus machine code, so it runs with the register VM's rungs too
  # (and must still agree with every other engine). With --superinstructions it compiles fused
  # words as their two halves (notes D16).
  add_test(NAME conformance-jit-superinstructions
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
            --rung $<TARGET_FILE:rung> --engine jit ${RUNG_JIT_ARGS} --superinstructions
            --timeout 120)
  add_test(NAME conformance-jit-all-rungs
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
            --rung $<TARGET_FILE:rung> --engine jit ${RUNG_JIT_ARGS} --superinstructions
            --inline-cache --fold --timeout 120)
  set_tests_properties(conformance-jit conformance-jit-gc-stress conformance-jit-fold
                       conformance-jit-superinstructions conformance-jit-all-rungs
                       PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Background compilation (Engine 5, notes D8): the whole suite with hot functions compiled on the
# JIT's compiler thread, in every build, ThreadSanitizer ones included; this is what the tsan CI
# jobs check the handoff and the GC protocol with (the tsan-goto-nanbox preset is the one with a
# JIT). With --jit-threshold=1 every function is queued on its first call, so the compiler thread
# is busy while the program runs, and with --gc-stress the collector runs on every allocation
# while the compiler thread reads the functions it has queued. Skipped where there is no JIT.
set(RUNG_JIT_BACKGROUND_ARGS --rung-arg=--jit-threshold=1 --rung-arg=--jit-background)
if(NOT RUNG_HAS_JIT)
  list(APPEND RUNG_JIT_BACKGROUND_ARGS --expect-unavailable)
endif()
add_test(NAME conformance-jit-background
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
          --rung $<TARGET_FILE:rung> --engine jit ${RUNG_JIT_BACKGROUND_ARGS} --timeout 300)
add_test(NAME conformance-jit-background-gc-stress
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
          --rung $<TARGET_FILE:rung> --engine jit ${RUNG_JIT_BACKGROUND_ARGS} --gc-stress
          --timeout 300 ${RUNG_GC_STRESS_SKIPS})
add_test(NAME conformance-jit-background-all-rungs
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_conformance.py
          --rung $<TARGET_FILE:rung> --engine jit ${RUNG_JIT_BACKGROUND_ARGS} --superinstructions
          --inline-cache --fold --timeout 300)
set_tests_properties(conformance-jit-background conformance-jit-background-gc-stress
                     conformance-jit-background-all-rungs PROPERTIES SKIP_RETURN_CODE 77)
