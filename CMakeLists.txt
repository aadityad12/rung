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
  src/diagnostic.cpp
  src/jit/arm64_emitter.cpp
  src/lexer.cpp
  src/token.cpp
)
target_include_directories(rung_core PUBLIC src)
target_link_libraries(rung_core PRIVATE rung_warnings)

add_executable(rung src/main.cpp)
target_link_libraries(rung PRIVATE rung_core rung_warnings)

# Unit tests (doctest, vendored single header).
enable_testing()
add_executable(rung_unit_tests
  tests/unit/test_main.cpp
  tests/unit/arm64_emitter_test.cpp
  tests/unit/lexer_test.cpp
)
target_include_directories(rung_unit_tests SYSTEM PRIVATE third_party/doctest)
target_link_libraries(rung_unit_tests PRIVATE rung_core rung_warnings)
add_test(NAME unit COMMAND rung_unit_tests)
