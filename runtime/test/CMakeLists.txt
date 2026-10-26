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

set(WTC_RT_SANITIZE "" CACHE STRING "Sanitizer to enable (thread, address or empty)")
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

# One element at a time with a pause between them: the consumer's slip must
# let each through within its bounded wait.
add_executable(spsc_ring_bursty_stress spsc_ring_stress.cpp)
target_link_libraries(spsc_ring_bursty_stress PRIVATE wtc_rt_flags)
target_compile_definitions(spsc_ring_bursty_stress PRIVATE WTC_STRESS_BURSTY=1)
add_test(NAME spsc_ring_bursty_stress COMMAND spsc_ring_bursty_stress 5000)

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

# The ring for one producer and many consumers, and its test that must
# fail: a second producer.
add_executable(spmc_ring_stress spmc_ring_stress.cpp)
target_link_libraries(spmc_ring_stress PRIVATE wtc_rt_flags)
add_test(NAME spmc_ring_stress COMMAND spmc_ring_stress 1000000)

add_executable(spmc_ring_two_producers spmc_ring_stress.cpp)
target_link_libraries(spmc_ring_two_producers PRIVATE wtc_rt_flags)
target_compile_definitions(spmc_ring_two_producers PRIVATE WTC_STRESS_TWO_PRODUCERS=1)
if(WTC_CHECK)
  add_test(NAME spmc_ring_two_producers_is_caught
           COMMAND sh -c "$<TARGET_FILE:spmc_ring_two_producers> 200000 2>&1 | grep -q 'SPMC contract violated'")
elseif(WTC_RT_SANITIZE STREQUAL "thread")
  add_test(NAME spmc_ring_two_producers_is_caught
           COMMAND sh -c "$<TARGET_FILE:spmc_ring_two_producers> 200000 2>&1 | grep -qE 'ThreadSanitizer: (data race|heap-use-after-free)'")
  set_tests_properties(spmc_ring_two_producers_is_caught PROPERTIES
                       ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:symbolize=0")
endif()

# The queue for one producer and many consumers that replaces the ring, with
# one, two and eight consumers, and its test that must fail: a second
# producer.
add_executable(spmc_queue_stress spmc_queue_stress.cpp)
target_link_libraries(spmc_queue_stress PRIVATE wtc_rt_flags)
foreach(consumers 1 2 8)
  add_test(NAME spmc_queue_stress_${consumers} COMMAND spmc_queue_stress 1000000 ${consumers})
endforeach()

# The element's destructor pushes to a general-purpose queue from inside the
# pop's taker; run it under AddressSanitizer (-DWTC_RT_SANITIZE=address) as
# well as ThreadSanitizer.
add_executable(spmc_queue_dtor_push spmc_queue_stress.cpp)
target_link_libraries(spmc_queue_dtor_push PRIVATE wtc_rt_flags)
target_compile_definitions(spmc_queue_dtor_push PRIVATE WTC_STRESS_DTOR_PUSH=1)
add_test(NAME spmc_queue_dtor_push COMMAND spmc_queue_dtor_push 1000000 8)

add_executable(spmc_queue_two_producers spmc_queue_stress.cpp)
target_link_libraries(spmc_queue_two_producers PRIVATE wtc_rt_flags)
target_compile_definitions(spmc_queue_two_producers PRIVATE WTC_STRESS_TWO_PRODUCERS=1)
if(WTC_CHECK)
  add_test(NAME spmc_queue_two_producers_is_caught
           COMMAND sh -c "$<TARGET_FILE:spmc_queue_two_producers> 200000 2>&1 | grep -q 'SPMC contract violated'")
elseif(WTC_RT_SANITIZE STREQUAL "thread")
  add_test(NAME spmc_queue_two_producers_is_caught
           COMMAND sh -c "$<TARGET_FILE:spmc_queue_two_producers> 200000 2>&1 | grep -qE 'ThreadSanitizer: (data race|heap-use-after-free)'")
  set_tests_properties(spmc_queue_two_producers_is_caught PROPERTIES
                       ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:symbolize=0")
endif()

# The segmented queue: many producers and many consumers, then the
# single-consumer form, and the latter's test that must fail: a second
# consumer.
add_executable(segment_queue_stress segment_queue_stress.cpp)
target_link_libraries(segment_queue_stress PRIVATE wtc_rt_flags)
add_test(NAME segment_queue_stress COMMAND segment_queue_stress 500000)

add_executable(segment_queue_one_consumer_stress segment_queue_stress.cpp)
target_link_libraries(segment_queue_one_consumer_stress PRIVATE wtc_rt_flags)
target_compile_definitions(segment_queue_one_consumer_stress PRIVATE WTC_STRESS_ONE_CONSUMER=1)
add_test(NAME segment_queue_one_consumer_stress COMMAND segment_queue_one_consumer_stress 500000)

add_executable(segment_queue_two_consumers segment_queue_stress.cpp)
target_link_libraries(segment_queue_two_consumers PRIVATE wtc_rt_flags)
target_compile_definitions(segment_queue_two_consumers PRIVATE WTC_STRESS_TWO_CONSUMERS=1)
if(WTC_CHECK)
  add_test(NAME segment_queue_two_consumers_is_caught
           COMMAND sh -c "$<TARGET_FILE:segment_queue_two_consumers> 200000 2>&1 | grep -q 'MPSC contract violated'")
elseif(WTC_RT_SANITIZE STREQUAL "thread")
  add_test(NAME segment_queue_two_consumers_is_caught
           COMMAND sh -c "$<TARGET_FILE:segment_queue_two_consumers> 200000 2>&1 | grep -qE 'ThreadSanitizer: (data race|heap-use-after-free)'")
  set_tests_properties(segment_queue_two_consumers_is_caught PROPERTIES
                       ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:symbolize=0")
endif()

# The empty answer with a push held between taking its cell and filling it:
# no form may answer empty while a later, finished push's element is queued.
add_executable(segment_queue_empty_test segment_queue_empty_test.cpp)
target_link_libraries(segment_queue_empty_test PRIVATE wtc_rt_flags)
add_test(NAME segment_queue_empty_test COMMAND segment_queue_empty_test)

# Producers that stop between taking a cell and filling it, for both forms;
# the single consumer checks each of its empty answers.
add_executable(segment_queue_stall_stress segment_queue_stress.cpp)
target_link_libraries(segment_queue_stall_stress PRIVATE wtc_rt_flags)
target_compile_definitions(segment_queue_stall_stress PRIVATE WTC_STRESS_STALL=1)
add_test(NAME segment_queue_stall_stress COMMAND segment_queue_stall_stress 200000)

add_executable(segment_queue_one_consumer_stall_stress segment_queue_stress.cpp)
target_link_libraries(segment_queue_one_consumer_stall_stress PRIVATE wtc_rt_flags)
target_compile_definitions(segment_queue_one_consumer_stall_stress PRIVATE WTC_STRESS_ONE_CONSUMER=1 WTC_STRESS_STALL=1)
add_test(NAME segment_queue_one_consumer_stall_stress COMMAND segment_queue_one_consumer_stall_stress 200000)

# The lemmas of docs/correctness.md, each on the interleaving its sketch
# relies on, forced by holding a thread at a named point (lemma_util.hpp).
add_executable(lemma_rings_test lemma_rings_test.cpp)
target_link_libraries(lemma_rings_test PRIVATE wtc_rt_flags)
add_test(NAME lemma_rings_test COMMAND lemma_rings_test 200000)

add_executable(lemma_segment_test lemma_segment_test.cpp)
target_link_libraries(lemma_segment_test PRIVATE wtc_rt_flags)
add_test(NAME lemma_segment_test COMMAND lemma_segment_test 100000)

# Section 7's claim that a substitution adds no wait: holds for the general
# queue, and does not for spmc, whose consumers wait for each other's move
# of the element. The spmc run passes only on that exact report, so that a
# race report or a wrong element is not mistaken for the known wait.
add_executable(lemma_spmc_liveness lemma_spmc_liveness.cpp)
target_link_libraries(lemma_spmc_liveness PRIVATE wtc_rt_flags)
add_test(NAME lemma_spmc_liveness_generic COMMAND lemma_spmc_liveness generic)
add_test(NAME lemma_spmc_liveness_spmc COMMAND lemma_spmc_liveness spmc)
set_tests_properties(lemma_spmc_liveness_spmc PROPERTIES
                     PASS_REGULAR_EXPRESSION "\\(spmc\\): a consumer waited"
                     FAIL_REGULAR_EXPRESSION "ThreadSanitizer|wrong element")
# A held thread that is never let go would hang the run.
set_tests_properties(lemma_rings_test lemma_segment_test lemma_spmc_liveness_generic
                     lemma_spmc_liveness_spmc PROPERTIES TIMEOUT 300)
