# The thread census: a preloaded library counting which threads take which
# mutex, and a self-test with a known answer.
#
#   cmake -S tools/census -B build-census && cmake --build build-census
#   cd build-census && ctest --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(wtc-census C)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
find_package(Threads REQUIRED)

add_library(wtc_threadcensus SHARED threadcensus.c)
target_compile_options(wtc_threadcensus PRIVATE -O2 -Wall -Wextra -Werror -fno-omit-frame-pointer)
target_link_libraries(wtc_threadcensus PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

add_executable(census_selftest selftest.c)
target_compile_options(census_selftest PRIVATE -O1 -Wall -Wextra -Werror)
target_link_libraries(census_selftest PRIVATE Threads::Threads)

enable_testing()
# Runs the self-test under the library and checks the two numbers the
# report must contain.
add_test(NAME census_selftest
  COMMAND ${CMAKE_COMMAND} -E env
    LD_PRELOAD=$<TARGET_FILE:wtc_threadcensus> WTC_CENSUS_OUT=${CMAKE_BINARY_DIR}/selftest.census
    $<TARGET_FILE:census_selftest>)
add_test(NAME census_selftest_report
  COMMAND grep -E "^single-thread mutexes: 1 of 2; their acquisitions: 1000 of 2000 \\(50\\.0%\\)" ${CMAKE_BINARY_DIR}/selftest.census)
set_tests_properties(census_selftest_report PROPERTIES DEPENDS census_selftest)
