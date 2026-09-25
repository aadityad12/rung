cmake_minimum_required(VERSION 3.20)
project(rung LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RUNG_WERROR "Treat compiler warnings as errors" ON)
set(RUNG_SANITIZE "" CACHE STRING "Sanitizers to build with: empty, 'address,undefined', or 'thread'")

if(RUNG_SANITIZE)
  add_compile_options(-fsanitize=${RUNG_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${RUNG_SANITIZE})
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
  src/ast_dump.cpp
  src/diagnostic.cpp
  src/jit/arm64_emitter.cpp
  src/lexer.cpp
  src/runtime/heap.cpp
  src/runtime/object.cpp
  src/parser.cpp
  src/token.cpp
)
target_include_directories(rung_core PUBLIC src)
target_link_libraries(rung_core PRIVATE rung_warnings)

# Executable memory only exists on arm64: an x86-64 CPU cannot run the machine code (notes D6).
set(RUNG_HAS_JIT_MEMORY OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(RUNG_HAS_JIT_MEMORY ON)
  target_sources(rung_core PRIVATE src/jit/exec_memory.cpp)
endif()

add_executable(rung src/main.cpp)
target_link_libraries(rung PRIVATE rung_core rung_warnings)

# Unit tests (doctest, vendored single header).
enable_testing()
add_executable(rung_unit_tests
  tests/unit/test_main.cpp
  tests/unit/arm64_emitter_test.cpp
  tests/unit/lexer_test.cpp
  tests/unit/runtime_heap_test.cpp
  tests/unit/parser_test.cpp
)
target_include_directories(rung_unit_tests SYSTEM PRIVATE third_party/doctest)
if(RUNG_HAS_JIT_MEMORY)
  target_sources(rung_unit_tests PRIVATE tests/unit/exec_memory_test.cpp)
endif()
target_link_libraries(rung_unit_tests PRIVATE rung_core rung_warnings)

# TEMPORARY, tsan only: the parser's "nesting limit" test parses a 5000-term flat expression,
# whose left-nested AST is destroyed recursively (a few native stack frames per term).
# ThreadSanitizer's much larger frames overflow the stack somewhere between 4000 and 5000 terms
# and the sanitizer aborts. This is a parser/AST robustness problem, not a JIT one; the test
# still runs in debug/release/asan. Remove this exclusion when the AST is destroyed without deep
# recursion.
set(RUNG_UNIT_TEST_ARGS)
if(RUNG_SANITIZE STREQUAL "thread")
  list(APPEND RUNG_UNIT_TEST_ARGS "--test-case-exclude=nesting limit")
endif()
add_test(NAME unit COMMAND rung_unit_tests ${RUNG_UNIT_TEST_ARGS})
