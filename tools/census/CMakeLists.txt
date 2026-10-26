# The thread census: a preloaded library counting which threads take which
# mutex and touch which shared_ptr control block, and self-tests with known
# answers.
#
#   cmake -S tools/census -B build-census && cmake --build build-census
#   cd build-census && ctest --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(wtc-census C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
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
  COMMAND grep -E "^single-thread mutexes: 3 of 4; their acquisitions: 1020 of 2020 \\(50\\.5%\\)" ${CMAKE_BINARY_DIR}/selftest.census)
set_tests_properties(census_selftest_report PROPERTIES DEPENDS census_selftest)

# Reference counts as wtc-cc --census reports them, by direct calls.
add_executable(census_selftest_rc selftest_rc.c)
target_compile_options(census_selftest_rc PRIVATE -O1 -Wall -Wextra -Werror)
target_link_libraries(census_selftest_rc PRIVATE wtc_threadcensus Threads::Threads)
add_test(NAME census_selftest_rc
  COMMAND ${CMAKE_COMMAND} -E env WTC_CENSUS_OUT=${CMAKE_BINARY_DIR}/selftest_rc.census
    $<TARGET_FILE:census_selftest_rc>)
add_test(NAME census_selftest_rc_report
  COMMAND grep -E "^single-thread control blocks: 200004 of 200006; their count operations: 201016 of 202018 \\(99\\.5%\\)" ${CMAKE_BINARY_DIR}/selftest_rc.census)
set_tests_properties(census_selftest_rc_report PROPERTIES DEPENDS census_selftest_rc)
set_tests_properties(census_selftest_rc PROPERTIES TIMEOUT 30)
# a, destroyed before the report, is still listed among the busiest
add_test(NAME census_selftest_rc_kept
  COMMAND grep -E "^0x[0-9a-f]+  1000  1$" ${CMAKE_BINARY_DIR}/selftest_rc.census)
set_tests_properties(census_selftest_rc_kept PROPERTIES DEPENDS census_selftest_rc)
# The same in a table of 4096 entries: 200,006 blocks fit only if retired
# slots are reused, and quickly only if probes stay short.
add_test(NAME census_selftest_rc_small
  COMMAND ${CMAKE_COMMAND} -E env WTC_CENSUS_TABLE=4096 WTC_CENSUS_OUT=${CMAKE_BINARY_DIR}/selftest_rc_small.census
    $<TARGET_FILE:census_selftest_rc>)
set_tests_properties(census_selftest_rc_small PROPERTIES TIMEOUT 30)
add_test(NAME census_selftest_rc_small_report
  COMMAND grep -E "^threadcensus: 200006 control blocks, 202018 count operations \\(0 on control blocks the table could not hold; table of 4096\\)" ${CMAKE_BINARY_DIR}/selftest_rc_small.census)
set_tests_properties(census_selftest_rc_small_report PROPERTIES DEPENDS census_selftest_rc_small)

# Reference counts through rc_hooks.h, in a program built without wtc-cc.
add_executable(census_selftest_rc_hooks selftest_rc_hooks.cpp)
target_include_directories(census_selftest_rc_hooks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(census_selftest_rc_hooks PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(census_selftest_rc_hooks PRIVATE Threads::Threads)
add_test(NAME census_selftest_rc_hooks
  COMMAND ${CMAKE_COMMAND} -E env
    LD_PRELOAD=$<TARGET_FILE:wtc_threadcensus> WTC_CENSUS_OUT=${CMAKE_BINARY_DIR}/selftest_rc_hooks.census
    $<TARGET_FILE:census_selftest_rc_hooks>)
add_test(NAME census_selftest_rc_hooks_report
  COMMAND grep -E "^single-thread control blocks: 1 of 2; their count operations: 1001 of 2002 \\(50\\.0%\\)" ${CMAKE_BINARY_DIR}/selftest_rc_hooks.census)
set_tests_properties(census_selftest_rc_hooks_report PROPERTIES DEPENDS census_selftest_rc_hooks)
