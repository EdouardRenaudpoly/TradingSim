cmake_minimum_required(VERSION 3.14)
project(MatchingEngine CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release flags: optimise + enable x86 intrinsics for RDTSC
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -march=native)
endif()

set(SQLITE_SRC ${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c)
if(EXISTS ${SQLITE_SRC})
    add_library(sqlite3_lib STATIC ${SQLITE_SRC})
    target_include_directories(sqlite3_lib PUBLIC ${CMAKE_SOURCE_DIR}/third_party/sqlite)
    set(HAVE_SQLITE3 TRUE)
    message(STATUS "SQLite: using vendored amalgamation (3.49.1)")
else()
    set(HAVE_SQLITE3 FALSE)
    message(STATUS "SQLite: not found, DB persistence disabled")
endif()

set(ENGINE_SOURCES
    engine/price_ladder.cpp
    engine/matching_engine.cpp
    engine/simulation.cpp
    engine/metrics.cpp
    engine/trade_store.cpp
)

add_library(engine_lib STATIC ${ENGINE_SOURCES})
target_include_directories(engine_lib PUBLIC ${CMAKE_SOURCE_DIR})

if(HAVE_SQLITE3)
    target_compile_definitions(engine_lib PUBLIC HAVE_SQLITE3)
    target_link_libraries(engine_lib PUBLIC sqlite3_lib)
endif()

add_executable(engine main.cpp)
target_link_libraries(engine PRIVATE engine_lib)

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
