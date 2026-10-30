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

# The ring's consumer decides whether to wait with a stream gate, tested
# here with tick sequences of its own.
add_executable(stream_gate_test stream_gate_test.cpp)
target_link_libraries(stream_gate_test PRIVATE wtc_rt_flags)
add_test(NAME stream_gate_test COMMAND stream_gate_test)

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

# The queue for one producer and many consumers, with one, two and eight
# consumers, and its test that must fail: a second producer.
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
add_executable(segment_queue_test segment_queue_test.cpp)
target_link_libraries(segment_queue_test PRIVATE wtc_rt_flags)
add_test(NAME segment_queue_test COMMAND segment_queue_test)

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

# Section 7's claim that a substitution adds no wait, for the general
# queue and for spmc, whose consumers move the element out after their
# claim, holding nothing another consumer waits on. The spmc run must pass
# outright; the report of a consumer that waited fails it whatever the
# exit status.
add_executable(lemma_spmc_liveness lemma_spmc_liveness.cpp)
target_link_libraries(lemma_spmc_liveness PRIVATE wtc_rt_flags)
add_test(NAME lemma_spmc_liveness_generic COMMAND lemma_spmc_liveness generic)
add_test(NAME lemma_spmc_liveness_spmc COMMAND lemma_spmc_liveness spmc)
set_tests_properties(lemma_spmc_liveness_spmc PROPERTIES
                     FAIL_REGULAR_EXPRESSION "\\(spmc\\): a consumer waited|ThreadSanitizer|wrong element")
# A held thread that is never let go would hang the run.
set_tests_properties(lemma_rings_test lemma_segment_test lemma_spmc_liveness_generic
                     lemma_spmc_liveness_spmc PROPERTIES TIMEOUT 300)

# The waiting pop (waiter.hpp) through the shim's queue, on every topology
# and wait policy: the lost wake-up's window forced, one element at a time
# to consumers that sleep, producers that pause at random, close() with
# sleepers, and the time limit. The shim's annotations are clang's; g++
# warns about them. The test that must fail: a waiter that drops its
# wake-ups must be caught sleeping through a publish.
add_executable(waiter_test waiter_test.cpp)
target_link_libraries(waiter_test PRIVATE wtc_rt_flags)
target_compile_options(waiter_test PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-attributes>)
if(WTC_RT_SANITIZE)
  set(waiter_pingpong 300)
  set(waiter_stress 10000)
else()
  set(waiter_pingpong 2000)
  set(waiter_stress 100000)
endif()
add_test(NAME waiter_interleave COMMAND waiter_test interleave)
add_test(NAME waiter_pingpong COMMAND waiter_test pingpong ${waiter_pingpong})
add_test(NAME waiter_stress COMMAND waiter_test stress ${waiter_stress})
add_test(NAME waiter_close COMMAND waiter_test close)
add_test(NAME waiter_timed COMMAND waiter_test timed)
set_tests_properties(waiter_interleave waiter_pingpong waiter_stress waiter_close waiter_timed
                     PROPERTIES TIMEOUT 300)

add_executable(waiter_drop_wakeups waiter_test.cpp)
target_link_libraries(waiter_drop_wakeups PRIVATE wtc_rt_flags)
target_compile_options(waiter_drop_wakeups PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-attributes>)
target_compile_definitions(waiter_drop_wakeups PRIVATE WTC_TEST_DROP_WAKEUPS=1)
add_test(NAME waiter_drop_wakeups_is_caught
         COMMAND sh -c "$<TARGET_FILE:waiter_drop_wakeups> interleave 2>&1 | grep -q 'lost wake-up: a publish'")

# The bounded queue (bounded_ring.hpp) through the shim's wtc::bounded:
# its answers against a sequential bounded queue, the exactness of its
# bound with a pop held inside its step, waiting on both sides on every
# topology and policy, close() with sleepers on both sides, and discards
# and overruns under contention. The test that must fail: waiters that
# drop their wake-ups must leave a producer asleep against room.
add_executable(bounded_test bounded_test.cpp)
target_link_libraries(bounded_test PRIVATE wtc_rt_flags)
if(WTC_RT_SANITIZE)
  set(bounded_n 2000)
  set(bounded_ping 300)
else()
  set(bounded_n 20000)
  set(bounded_ping 2000)
endif()
add_test(NAME bounded_model COMMAND bounded_test model ${bounded_n})
add_test(NAME bounded_exact COMMAND bounded_test exact)
add_test(NAME bounded_stress COMMAND bounded_test stress ${bounded_n})
add_test(NAME bounded_pingfull COMMAND bounded_test pingfull ${bounded_ping})
add_test(NAME bounded_close COMMAND bounded_test close)
add_test(NAME bounded_discard COMMAND bounded_test discard ${bounded_n})
add_test(NAME bounded_overrun COMMAND bounded_test overrun ${bounded_n})
set_tests_properties(bounded_model bounded_exact bounded_stress bounded_pingfull bounded_close
                     bounded_discard bounded_overrun PROPERTIES TIMEOUT 300)

add_executable(bounded_drop_wakeups bounded_test.cpp)
target_link_libraries(bounded_drop_wakeups PRIVATE wtc_rt_flags)
target_compile_definitions(bounded_drop_wakeups PRIVATE WTC_TEST_DROP_WAKEUPS=1)
add_test(NAME bounded_drop_wakeups_is_caught
         COMMAND sh -c "$<TARGET_FILE:bounded_drop_wakeups> pingfull 200 2>&1 | grep -q 'lost wake-up: a publish woke nobody'")

# A push of the split layout held between its two reads while other
# threads move both counters past it: it must not be refused as full.
add_executable(bounded_push_steps bounded_test.cpp)
target_link_libraries(bounded_push_steps PRIVATE wtc_rt_flags)
target_compile_definitions(bounded_push_steps PRIVATE WTC_TEST_PUSH_STEPS=1)
add_test(NAME bounded_split_push_steps COMMAND bounded_push_steps split)
set_tests_properties(bounded_split_push_steps PROPERTIES TIMEOUT 300)

# Two producers on a queue built for one: the contract check must say so.
if(WTC_CHECK)
  add_test(NAME bounded_two_producers_is_caught
           COMMAND sh -c "$<TARGET_FILE:bounded_test> twoproducers 2>&1 | grep -q 'bounded queue contract violated: a second producer'")
endif()

# The runtime of lifted C queues (shim/c_queue.cpp), through the entry
# points a lifted program calls.
add_executable(c_queue_test c_queue_test.cpp ../../shim/c_queue.cpp)
target_link_libraries(c_queue_test PRIVATE wtc_rt_flags)
if(WTC_RT_SANITIZE)
  set(c_queue_n 2000)
else()
  set(c_queue_n 20000)
endif()
add_test(NAME c_queue_model COMMAND c_queue_test model ${c_queue_n})
add_test(NAME c_queue_stress COMMAND c_queue_test stress ${c_queue_n})
set_tests_properties(c_queue_model c_queue_stress PROPERTIES TIMEOUT 300)
# The same with a hook between a batch's size and its claim, to cross two.
add_executable(c_queue_crossing c_queue_test.cpp ../../shim/c_queue.cpp)
target_link_libraries(c_queue_crossing PRIVATE wtc_rt_flags)
target_compile_definitions(c_queue_crossing PRIVATE WTC_TEST_BATCH_STEPS=1)
add_test(NAME c_queue_crossing COMMAND c_queue_crossing crossing)
set_tests_properties(c_queue_crossing PROPERTIES TIMEOUT 60)
if(WTC_CHECK)
  add_test(NAME c_queue_two_consumers_is_caught
           COMMAND sh -c "$<TARGET_FILE:c_queue_test> twoconsumers 2>&1 | grep -q 'bounded queue contract violated: a second consumer'")
endif()
