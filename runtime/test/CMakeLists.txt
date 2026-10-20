# Standalone: needs no LLVM. Configure with the system compiler, e.g.
#   cmake -S runtime/test -B build-rt -DCMAKE_CXX_COMPILER=clang++-18
#   cmake --build build-rt && ctest --test-dir build-rt
# Add -DWTC_RT_SANITIZE=thread for the ThreadSanitizer variant. The pinned
# ClangIR clang has no sanitizer runtime, so use clang-18 or g++-13 here;
# g++-13's TSan needs address randomisation off (setarch x86_64 -R).
cmake_minimum_required(VERSION 3.20)
project(wtc-runtime-tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(WTC_RT_SANITIZE "" CACHE STRING "Sanitizer to enable (thread or empty)")
option(WTC_CHECK "Build the runtime with its contract checks" OFF)

add_library(wtc_rt_flags INTERFACE)
target_include_directories(wtc_rt_flags INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(wtc_rt_flags INTERFACE -Wall -Wextra -Werror)
if(WTC_RT_SANITIZE)
  target_compile_options(wtc_rt_flags INTERFACE -fsanitize=${WTC_RT_SANITIZE} -O1 -g)
  target_link_options(wtc_rt_flags INTERFACE -fsanitize=${WTC_RT_SANITIZE})
endif()
if(WTC_CHECK)
  target_compile_definitions(wtc_rt_flags INTERFACE WTC_CHECK=1)
endif()
find_package(Threads REQUIRED)
target_link_libraries(wtc_rt_flags INTERFACE Threads::Threads)

enable_testing()

add_executable(spsc_ring_test spsc_ring_test.cpp)
target_link_libraries(spsc_ring_test PRIVATE wtc_rt_flags)
add_test(NAME spsc_ring_test COMMAND spsc_ring_test)

add_executable(spsc_ring_stress spsc_ring_stress.cpp)
target_link_libraries(spsc_ring_stress PRIVATE wtc_rt_flags)
add_test(NAME spsc_ring_stress COMMAND spsc_ring_stress 2000000)

# The same, with elements held inline and borrowed in place by the consumer.
add_executable(spsc_ring_borrow_stress spsc_ring_stress.cpp)
target_link_libraries(spsc_ring_borrow_stress PRIVATE wtc_rt_flags)
target_compile_definitions(spsc_ring_borrow_stress PRIVATE WTC_STRESS_BORROW=1)
add_test(NAME spsc_ring_borrow_stress COMMAND spsc_ring_borrow_stress 2000000)

# The same, publishing once per run of pushes.
add_executable(spsc_ring_deferred_stress spsc_ring_stress.cpp)
target_link_libraries(spsc_ring_deferred_stress PRIVATE wtc_rt_flags)
target_compile_definitions(spsc_ring_deferred_stress PRIVATE WTC_STRESS_DEFERRED=1)
add_test(NAME spsc_ring_deferred_stress COMMAND spsc_ring_deferred_stress 2000000)

# The test that proves the tests can fail: two producers must be caught, and
# the test asserts the exact message of the catcher rather than "it failed
# somehow". With WTC_CHECK the catcher is the contract check, which aborts;
# ctest counts an abort as a crash even under WILL_FAIL, so the message is
# grepped through a shell. Under TSan the catcher is the race report, and
# halt_on_error stops the run at the first one instead of letting a
# sanitized two-producer run crawl to the end.
add_executable(spsc_ring_two_producers spsc_ring_stress.cpp)
target_link_libraries(spsc_ring_two_producers PRIVATE wtc_rt_flags)
target_compile_definitions(spsc_ring_two_producers PRIVATE WTC_STRESS_TWO_PRODUCERS=1)
if(WTC_CHECK)
  add_test(NAME spsc_ring_two_producers_is_caught
           COMMAND sh -c "$<TARGET_FILE:spsc_ring_two_producers> 200000 2>&1 | grep -q 'SPSC contract violated'")
elseif(WTC_RT_SANITIZE STREQUAL "thread")
  add_test(NAME spsc_ring_two_producers_is_caught
           COMMAND sh -c "$<TARGET_FILE:spsc_ring_two_producers> 200000 2>&1 | grep -q 'ThreadSanitizer: data race'")
  # symbolize=0: on this host llvm-symbolizer-18 hangs for three minutes on
  # the "[stack]" mapping before the report is printed, with the race long
  # since detected. The message is all this test needs.
  set_tests_properties(spsc_ring_two_producers_is_caught PROPERTIES
                       ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:symbolize=0")
endif()

# The list for many producers and one consumer, and its test that must
# fail: a second consumer, caught as the ring's second producer is.
add_executable(mpsc_list_stress mpsc_list_stress.cpp)
target_link_libraries(mpsc_list_stress PRIVATE wtc_rt_flags)
add_test(NAME mpsc_list_stress COMMAND mpsc_list_stress 1000000)

add_executable(mpsc_list_two_consumers mpsc_list_stress.cpp)
target_link_libraries(mpsc_list_two_consumers PRIVATE wtc_rt_flags)
target_compile_definitions(mpsc_list_two_consumers PRIVATE WTC_STRESS_TWO_CONSUMERS=1)
if(WTC_CHECK)
  add_test(NAME mpsc_list_two_consumers_is_caught
           COMMAND sh -c "$<TARGET_FILE:mpsc_list_two_consumers> 200000 2>&1 | grep -q 'MPSC contract violated'")
elseif(WTC_RT_SANITIZE STREQUAL "thread")
  add_test(NAME mpsc_list_two_consumers_is_caught
           COMMAND sh -c "$<TARGET_FILE:mpsc_list_two_consumers> 200000 2>&1 | grep -q 'ThreadSanitizer: data race'")
  set_tests_properties(mpsc_list_two_consumers_is_caught PROPERTIES
                       ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:symbolize=0")
endif()
