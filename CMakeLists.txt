cmake_minimum_required(VERSION 3.14)
project(MatchingEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release flags: optimise + enable x86 intrinsics for RDTSC
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native)
endif()

# ── Engine library ─────────────────────────────────────────────────────────────
set(ENGINE_SOURCES
    engine/price_ladder.cpp
    engine/matching_engine.cpp
    engine/simulation.cpp
    engine/metrics.cpp
)

add_library(engine_lib STATIC ${ENGINE_SOURCES})
target_include_directories(engine_lib PUBLIC ${CMAKE_SOURCE_DIR})

# ── CLI / benchmark executable ─────────────────────────────────────────────────
add_executable(engine main.cpp)
target_link_libraries(engine PRIVATE engine_lib)

# ── Tests ──────────────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

file(GLOB TEST_SOURCES tests/*.cpp)
add_executable(tests ${TEST_SOURCES})
target_link_libraries(tests PRIVATE engine_lib GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(tests)
