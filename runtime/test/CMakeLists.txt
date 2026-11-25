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
# A push whose element's move or whose segment's allocation throws: the
# cells it took are given up, and no pop waits on them.
add_executable(throw_test throw_test.cpp)
target_link_libraries(throw_test PRIVATE wtc_rt_flags)
foreach(mode generic batch mpsc jiffy hole hole_jiffy)
  add_test(NAME throw_${mode} COMMAND throw_test ${mode})
  set_tests_properties(throw_${mode} PROPERTIES TIMEOUT 60)  # a pop that waits on a given-up cell hangs
endforeach()
if(NOT WTC_RT_SANITIZE)
  target_compile_definitions(throw_test PRIVATE WTC_TEST_FAIL_NEW=1)
  add_test(NAME throw_alloc COMMAND throw_test alloc)
  add_test(NAME throw_alloc_jiffy COMMAND throw_test alloc_jiffy)
endif()

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

# The single consumer's pop bounded by the reservations in flight: after a
# long-held push fills, no pop makes more than a few looks or retires more
# than one segment, and its time does not grow with the cells taken while
# it was held; two held pushes far apart; looks per pop against the
# producers pausing at random.
add_executable(mpsc_bound_test mpsc_bound_test.cpp)
target_link_libraries(mpsc_bound_test PRIVATE wtc_rt_flags)
add_test(NAME mpsc_bound_late_fill COMMAND mpsc_bound_test late_fill)
add_test(NAME mpsc_bound_two_holes COMMAND mpsc_bound_test two_holes)
# And the same with the one-counter push (mpsc_jiffy_bound_test), whose
# steps to its index's segment push_steps counts.
add_executable(mpsc_jiffy_bound_test mpsc_bound_test.cpp)
target_link_libraries(mpsc_jiffy_bound_test PRIVATE wtc_rt_flags)
target_compile_definitions(mpsc_jiffy_bound_test PRIVATE WTC_TEST_ONE_COUNTER=1)
add_test(NAME mpsc_jiffy_bound_two_holes COMMAND mpsc_jiffy_bound_test two_holes)
foreach(p 1 4 16)
  add_test(NAME mpsc_bound_holes_${p} COMMAND mpsc_bound_test holes ${p} 400000)
  add_test(NAME mpsc_jiffy_bound_holes_${p} COMMAND mpsc_jiffy_bound_test holes ${p} 400000)
  add_test(NAME mpsc_bound_push_steps_${p} COMMAND mpsc_jiffy_bound_test push_steps ${p} 100000)
  set_tests_properties(mpsc_bound_push_steps_${p} mpsc_jiffy_bound_holes_${p} PROPERTIES TIMEOUT 300)
endforeach()
set_tests_properties(mpsc_bound_late_fill mpsc_bound_two_holes mpsc_bound_holes_1 mpsc_bound_holes_4
                     mpsc_bound_holes_16 PROPERTIES TIMEOUT 300)
# late_fill compares a pop's time with the first one's: alone, so that
# the other tests' load does not make one pop slow.
set_tests_properties(mpsc_bound_late_fill PROPERTIES RUN_SERIAL TRUE)
# A hundred million elements past a push stopped for ever: the segments'
# memory stays at a handful (fold, the one-counter form's).
if(WTC_RT_SANITIZE)
  set(fold_n 200000)
else()
  set(fold_n 100000000)
endif()
add_test(NAME mpsc_bound_fold COMMAND mpsc_jiffy_bound_test fold ${fold_n})
set_tests_properties(mpsc_bound_fold PROPERTIES TIMEOUT 600)

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
# The same two with the one-counter push.
add_executable(segment_queue_jiffy_stress segment_queue_stress.cpp)
target_link_libraries(segment_queue_jiffy_stress PRIVATE wtc_rt_flags)
target_compile_definitions(segment_queue_jiffy_stress PRIVATE WTC_STRESS_ONE_CONSUMER=1 WTC_STRESS_ONE_COUNTER=1)
add_test(NAME segment_queue_jiffy_stress COMMAND segment_queue_jiffy_stress 500000)
add_executable(segment_queue_jiffy_stall_stress segment_queue_stress.cpp)
target_link_libraries(segment_queue_jiffy_stall_stress PRIVATE wtc_rt_flags)
target_compile_definitions(segment_queue_jiffy_stall_stress PRIVATE WTC_STRESS_ONE_CONSUMER=1 WTC_STRESS_ONE_COUNTER=1
                                                                    WTC_STRESS_STALL=1)
add_test(NAME segment_queue_jiffy_stall_stress COMMAND segment_queue_jiffy_stall_stress 200000)

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

# The wait whose loop is the caller's (waiter.hpp: begin, step, end), as a
# lifted wait uses it (shim/lifted.hpp): a predicate of the program's own,
# a pop from a queue under its own mutex or a stop flag, on every policy;
# the lost wake-up's window forced, tasks with pauses then the stop flag,
# every waiter asleep then the stop flag. The test that must fail: a
# waiter that drops its wake-ups must be caught sleeping through a publish.
add_executable(waiter_loop_test waiter_loop_test.cpp)
target_link_libraries(waiter_loop_test PRIVATE wtc_rt_flags)
target_compile_options(waiter_loop_test PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-attributes>)
if(WTC_RT_SANITIZE)
  set(waiter_loop_stress 2000)
else()
  set(waiter_loop_stress 20000)
endif()
add_test(NAME waiter_loop_interleave COMMAND waiter_loop_test interleave)
add_test(NAME waiter_loop_stress COMMAND waiter_loop_test stress ${waiter_loop_stress})
add_test(NAME waiter_loop_stop COMMAND waiter_loop_test stop)
add_test(NAME waiter_loop_idle COMMAND waiter_loop_test idle)
set_tests_properties(waiter_loop_interleave waiter_loop_stress waiter_loop_stop waiter_loop_idle PROPERTIES TIMEOUT 300)

# The policy rule of a lifted wait (shim/lifted.hpp), on affinity masks
# the test sets: counts not known never spin, whatever the mask.
add_executable(lifted_policy_test lifted_policy_test.cpp)
target_link_libraries(lifted_policy_test PRIVATE wtc_rt_flags)
target_compile_options(lifted_policy_test PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-attributes>)
add_test(NAME lifted_policy COMMAND lifted_policy_test)

add_executable(waiter_loop_drop_wakeups waiter_loop_test.cpp)
target_link_libraries(waiter_loop_drop_wakeups PRIVATE wtc_rt_flags)
target_compile_options(waiter_loop_drop_wakeups PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-attributes>)
target_compile_definitions(waiter_loop_drop_wakeups PRIVATE WTC_TEST_DROP_WAKEUPS=1)
add_test(NAME waiter_loop_drop_wakeups_is_caught
         COMMAND sh -c "$<TARGET_FILE:waiter_loop_drop_wakeups> interleave 2>&1 | grep -q 'lost wake-up: a publish'")

# The bounded queue (bounded_ring.hpp) through the shim's wtc::bounded:
# its answers against a sequential bounded queue, the exactness of its
# bound with a pop held inside its step, waiting on both sides on every
# topology and policy, close() with sleepers on both sides, and discards
# and overruns under contention. The test that must fail: waiters that
# drop their wake-ups must leave a producer asleep against room.
add_executable(bounded_test bounded_test.cpp)
target_link_libraries(bounded_test PRIVATE wtc_rt_flags)
# The shim's annotations are clang's; and the wcq tag runs the wait-free
# ring only where the double-width compare-and-swap is one instruction.
target_compile_options(bounded_test PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-attributes>)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  target_compile_options(bounded_test PRIVATE -mcx16)
endif()
if(WTC_RT_SANITIZE)
  set(bounded_n 2000)
  set(bounded_ping 300)
else()
  set(bounded_n 20000)
  set(bounded_ping 2000)
endif()
add_test(NAME bounded_model COMMAND bounded_test model ${bounded_n})
add_test(NAME bounded_exact COMMAND bounded_test exact)
add_test(NAME bounded_sides COMMAND bounded_test sides)
add_test(NAME bounded_stress COMMAND bounded_test stress ${bounded_n})
add_test(NAME bounded_pingfull COMMAND bounded_test pingfull ${bounded_ping})
add_test(NAME bounded_close COMMAND bounded_test close)
add_test(NAME bounded_discard COMMAND bounded_test discard ${bounded_n})
add_test(NAME bounded_overrun COMMAND bounded_test overrun ${bounded_n})
set_tests_properties(bounded_model bounded_exact bounded_sides bounded_stress bounded_pingfull bounded_close
                     bounded_discard bounded_overrun PROPERTIES TIMEOUT 300)

add_executable(bounded_drop_wakeups bounded_test.cpp)
target_link_libraries(bounded_drop_wakeups PRIVATE wtc_rt_flags)
target_compile_options(bounded_drop_wakeups PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-attributes>)
target_compile_definitions(bounded_drop_wakeups PRIVATE WTC_TEST_DROP_WAKEUPS=1)
add_test(NAME bounded_drop_wakeups_is_caught
         COMMAND sh -c "$<TARGET_FILE:bounded_drop_wakeups> pingfull 200 2>&1 | grep -q 'lost wake-up: a publish woke nobody'")

# A push of the split layout held between its two reads while other
# threads move both counters past it: it must not be refused as full.
add_executable(bounded_push_steps bounded_test.cpp)
target_link_libraries(bounded_push_steps PRIVATE wtc_rt_flags)
target_compile_options(bounded_push_steps PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-attributes>)
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
add_test(NAME c_queue_consumers COMMAND c_queue_test consumers ${c_queue_n} 14)
add_test(NAME c_queue_close COMMAND c_queue_test close)
add_test(NAME c_queue_overterminate_is_caught
         COMMAND sh -c "$<TARGET_FILE:c_queue_test> overterminate 2>&1 | grep -q 'terminated more often than the producers'")
add_test(NAME c_queue_atleast COMMAND c_queue_test atleast)
add_test(NAME c_queue_limit COMMAND c_queue_test limit)
add_test(NAME c_queue_absorb COMMAND c_queue_test absorb)
add_test(NAME c_queue_zero_slots COMMAND c_queue_test zeroslots)
set_tests_properties(c_queue_model c_queue_stress c_queue_consumers c_queue_close PROPERTIES TIMEOUT 300)
set_tests_properties(c_queue_atleast c_queue_limit c_queue_absorb c_queue_zero_slots PROPERTIES TIMEOUT 60)
# The same with a hook where the last termination has finished the queue,
# to destroy it from the consumer while the termination is still inside.
add_executable(c_queue_destroy c_queue_test.cpp ../../shim/c_queue.cpp)
target_link_libraries(c_queue_destroy PRIVATE wtc_rt_flags)
target_compile_definitions(c_queue_destroy PRIVATE WTC_C_QUEUE_TEST_HOOK=1)
add_test(NAME c_queue_destroy COMMAND c_queue_destroy destroy)
set_tests_properties(c_queue_destroy PROPERTIES TIMEOUT 60)
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

# The delegation server (delegate.hpp) through the entry point a delegated
# program calls (shim/delegate.cpp): exclusion between clients and threads
# that run a section inline, results and writes coming back, each client's
# order, a section that reaches another delegated one, one server per
# mutex, clients and servers asleep between sections, and short-lived
# clients whose waiters later threads take over. These run with every
# section delegated (WTC_DELEGATE_MODE=always), and those that do not
# depend on it again in the default, adaptive, mode (delegate_adaptive_*).
add_executable(delegate_test delegate_test.cpp ../../shim/delegate.cpp)
target_link_libraries(delegate_test PRIVATE wtc_rt_flags)
if(WTC_RT_SANITIZE)
  set(delegate_n 5000)
  set(delegate_park 300)
  set(delegate_churn 30)
else()
  set(delegate_n 100000)
  set(delegate_park 2000)
  set(delegate_churn 200)
endif()
set(delegate_always)
set(delegate_adaptive)
foreach(mode exclusion order servers)
  add_test(NAME delegate_${mode} COMMAND delegate_test ${mode} ${delegate_n})
  list(APPEND delegate_always delegate_${mode})
endforeach()
add_test(NAME delegate_park COMMAND delegate_test park ${delegate_park})
add_test(NAME delegate_churn COMMAND delegate_test churn ${delegate_churn})
list(APPEND delegate_always delegate_park delegate_churn)
# The servers' lives and how they are started: they end with the last
# client (and a main that ends with pthread_exit ends the process), at most
# four run, they block every signal, have their starting client's stack,
# start afresh in a fork's child, and a section from a thread-local
# destructor after the thread stopped being a client runs on it.
foreach(mode results nested ends cap signals stack fork tls_exit errno)
  add_test(NAME delegate_${mode} COMMAND delegate_test ${mode})
  list(APPEND delegate_always delegate_${mode})
endforeach()
foreach(mode exclusion order)
  add_test(NAME delegate_adaptive_${mode} COMMAND delegate_test ${mode} ${delegate_n})
  list(APPEND delegate_adaptive delegate_adaptive_${mode})
endforeach()
add_test(NAME delegate_adaptive_results COMMAND delegate_test results)
add_test(NAME delegate_adaptive_park COMMAND delegate_test park ${delegate_park})
add_test(NAME delegate_adaptive_churn COMMAND delegate_test churn ${delegate_churn})
list(APPEND delegate_adaptive delegate_adaptive_results delegate_adaptive_park delegate_adaptive_churn)
# Where the adaptive runtime runs a section: a free mutex on its client
# with no server; a contended one as written, then delegated once hot, then
# as written again once cooled, its server started on the way and ending
# with the clients; with WTC_DELEGATE_HOT_NS=0, every contended section
# delegated; the three ways mixed with threads outside the runtime; a
# fork's child starting cold; the gate's average at two counter rates and
# with a threshold of 60 ns; a cooled mutex's server ending and its place
# going to the next hot one, and a hot one that can have none turning
# cold; gates for colliding mutexes, up to 512; a mutex contended 1% of
# the time staying cold, also from a thread that goes between it and
# another; and a mutex hot in a burst and then idle giving its server up.
add_test(NAME delegate_free COMMAND delegate_test free ${delegate_n})
add_test(NAME delegate_mix COMMAND delegate_test mix ${delegate_n})
foreach(mode contended trylock fork_cold average small_hot slots gates rare rare_alt idle_hot)
  add_test(NAME delegate_${mode} COMMAND delegate_test ${mode})
  list(APPEND delegate_adaptive delegate_${mode})
endforeach()
list(APPEND delegate_adaptive delegate_free delegate_mix)
set_property(TEST ${delegate_always} APPEND PROPERTY ENVIRONMENT "WTC_DELEGATE_MODE=always")
set_property(TEST delegate_trylock APPEND PROPERTY ENVIRONMENT "WTC_DELEGATE_HOT_NS=0")
set_property(TEST delegate_contended APPEND PROPERTY ENVIRONMENT "WTC_DELEGATE_EPOCH=4")
set_property(TEST delegate_mix APPEND PROPERTY ENVIRONMENT "WTC_DELEGATE_HOT_NS=200;WTC_DELEGATE_EPOCH=256")
set_property(TEST delegate_small_hot APPEND PROPERTY ENVIRONMENT "WTC_DELEGATE_HOT_NS=60")
set_property(TEST delegate_slots APPEND PROPERTY ENVIRONMENT "WTC_DELEGATE_MAX_SERVERS=1;WTC_DELEGATE_EPOCH=64")
set_property(TEST delegate_idle_hot APPEND PROPERTY ENVIRONMENT "WTC_DELEGATE_MAX_SERVERS=1")
# The first section reads the environment: an out-of-range number there
# leaves errno as it was.
add_test(NAME delegate_adaptive_errno COMMAND delegate_test errno)
set_property(TEST delegate_adaptive_errno APPEND PROPERTY ENVIRONMENT
             "WTC_DELEGATE_HOT_NS=99999999999999999999999;WTC_DELEGATE_EPOCH=99999999999999999999999")
list(APPEND delegate_adaptive delegate_adaptive_errno)
set_tests_properties(${delegate_always} ${delegate_adaptive} PROPERTIES TIMEOUT 300)
# ThreadSanitizer's own runtime keeps a process whose main thread ended with
# pthread_exit alive (a program with no server does the same), and refuses
# to start a thread in the child of a multi-threaded fork unless told.
if(NOT WTC_RT_SANITIZE STREQUAL "thread")
  # Many forks of a process whose threads are running: ThreadSanitizer's own
  # runtime is not fork-safe that way.
  add_test(NAME delegate_fork_statics COMMAND delegate_test fork_statics)
  set_tests_properties(delegate_fork_statics PROPERTIES TIMEOUT 60)
  add_test(NAME delegate_main_exit COMMAND delegate_test main_exit)
  add_test(NAME delegate_adaptive_main_exit COMMAND delegate_test main_exit)
  set_tests_properties(delegate_main_exit delegate_adaptive_main_exit PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "main_exit: ok")
  set_property(TEST delegate_main_exit APPEND PROPERTY ENVIRONMENT "WTC_DELEGATE_MODE=always")
else()
  set_property(TEST delegate_fork delegate_fork_cold APPEND PROPERTY ENVIRONMENT "TSAN_OPTIONS=die_after_fork=0")
endif()

# What a thread's and the process's first operation does beyond its
# algorithm: allocator calls (the allocator interposed), guarded static
# initialisations, thread-exit registrations and mutexes (the entry points
# wrapped at link time). Not under a sanitizer, whose interceptors would
# be counted too.
if(NOT WTC_RT_SANITIZE)
  add_executable(first_use_test first_use_test.cpp)
  target_link_libraries(first_use_test PRIVATE wtc_rt_flags)
  target_link_options(first_use_test PRIVATE -Wl,--wrap=__cxa_guard_acquire
                      -Wl,--wrap=__cxa_thread_atexit -Wl,--wrap=pthread_mutex_lock)
  add_test(NAME first_use_tick_rate COMMAND first_use_test tick_rate)
  add_test(NAME first_use_first_ops COMMAND first_use_test first_ops)
  # The hazard key past glibc's first 32 keys, with its allocation and
  # with that allocation failing.
  add_test(NAME first_use_keys40 COMMAND first_use_test keys40)
  add_test(NAME first_use_enomem COMMAND first_use_test enomem)
  add_test(NAME first_use_retire COMMAND first_use_test retire)
  add_test(NAME first_use_index_queue COMMAND first_use_test index_queue)
  # The wait-free ring's queue too, and as the process's first hazard user.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(first_use_test PRIVATE -mcx16)
    target_compile_definitions(first_use_test PRIVATE WTC_TEST_WCQ_FIRST_USE=1)
    add_test(NAME first_use_wcq_first COMMAND first_use_test wcq_first)
    add_test(NAME first_use_untied COMMAND first_use_test untied)
  endif()
endif()

# The bounded lock-free queue over rings of indices (index_ring.hpp), both
# variants: answers against a sequential queue, three-thread histories
# searched for a linearisation, a pop held inside its move, producers and
# consumers frozen inside their operations while the others go on, and
# stress. The model-checking configurations are in model/ (run.sh), run
# under GenMC where it is installed, not here.
add_executable(index_queue_test index_queue_test.cpp)
target_link_libraries(index_queue_test PRIVATE wtc_rt_flags)
if(WTC_RT_SANITIZE)
  set(index_lin 2000)
  set(index_stress 20000)
  set(index_ring 200)
else()
  set(index_lin 20000)
  set(index_stress 200000)
  set(index_ring 2000)
endif()
add_test(NAME index_queue_model COMMAND index_queue_test model 20000)
add_test(NAME index_queue_lin COMMAND index_queue_test lin ${index_lin})
add_test(NAME index_queue_held COMMAND index_queue_test held)
add_test(NAME index_queue_frozen COMMAND index_queue_test frozen)
add_test(NAME index_queue_stress COMMAND index_queue_test stress ${index_stress})
add_test(NAME index_queue_boundary COMMAND index_queue_test boundary)
add_test(NAME index_queue_inflight COMMAND index_queue_test inflight)
add_test(NAME index_queue_ring COMMAND index_queue_test ring ${index_ring})
set_tests_properties(index_queue_model index_queue_lin index_queue_held index_queue_frozen
                     index_queue_stress index_queue_ring PROPERTIES TIMEOUT 600)
# The ring churn alone: with the other tests' threads on the processors
# its own interleave less, and a threshold too small shows less often.
set_tests_properties(index_queue_ring PROPERTIES RUN_SERIAL TRUE)

# The same tests on the wait-free ring (wcq_ring), with its defaults and in
# the build in which every enqueue and almost every dequeue takes the slow
# path and every operation helps; and the wait-free ring's own tests:
# threads frozen at each point of their operations while the others go on
# (their steps printed), a ring churned by six threads, and the detection
# of the double-width compare-and-swap, which a build without -mcx16 must
# report missing. x86-64 only here: AArch64 needs LSE (casp) instead.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  foreach(v 1 2)
    if(v EQUAL 1)
      set(name index_queue_wcq_test)
    else()
      set(name index_queue_wcq_slow_test)
    endif()
    add_executable(${name} index_queue_test.cpp)
    target_link_libraries(${name} PRIVATE wtc_rt_flags)
    target_compile_options(${name} PRIVATE -mcx16)
    target_compile_definitions(${name} PRIVATE WTC_TEST_WCQ=${v})
    foreach(mode model held frozen)
      add_test(NAME ${name}_${mode} COMMAND ${name} ${mode} 20000)
    endforeach()
    foreach(mode boundary inflight)
      add_test(NAME ${name}_${mode} COMMAND ${name} ${mode})
    endforeach()
    add_test(NAME ${name}_ring COMMAND ${name} ring ${index_ring})
    add_test(NAME ${name}_lin COMMAND ${name} lin ${index_lin})
    add_test(NAME ${name}_stress COMMAND ${name} stress ${index_stress})
    set_tests_properties(${name}_model ${name}_held ${name}_frozen ${name}_lin ${name}_stress ${name}_ring
                         PROPERTIES TIMEOUT 900)
  endforeach()
  if(WTC_RT_SANITIZE)
    set(wcq_frozen 2000)
    set(wcq_ring 30)
    set(wcq_slow_test_ring 30)
  else()
    set(wcq_frozen 20000)
    set(wcq_ring 1000)
    # The lookup that stops at the first counter matching under its flags
    # loses or doubles an index about once in a thousand rounds here.
    set(wcq_slow_test_ring 10000)
  endif()
  add_executable(wcq_test wcq_test.cpp)
  target_link_libraries(wcq_test PRIVATE wtc_rt_flags)
  target_compile_options(wcq_test PRIVATE -mcx16)
  add_executable(wcq_slow_test wcq_test.cpp)
  target_link_libraries(wcq_slow_test PRIVATE wtc_rt_flags)
  target_compile_options(wcq_slow_test PRIVATE -mcx16)
  target_compile_definitions(wcq_slow_test PRIVATE WTC_TEST_WCQ_SLOW=1)
  set(wcq_test_ring ${wcq_ring})
  foreach(t wcq_test wcq_slow_test)
    add_test(NAME ${t}_frozen COMMAND ${t} frozen ${wcq_frozen})
    add_test(NAME ${t}_ring COMMAND ${t} ring ${${t}_ring})
    add_test(NAME ${t}_cas2 COMMAND ${t} cas2)
    add_test(NAME ${t}_slots COMMAND ${t} slots)
    add_test(NAME ${t}_fork COMMAND ${t} fork)
    add_test(NAME ${t}_helped COMMAND ${t} helped)
    if(NOT WTC_RT_SANITIZE STREQUAL "thread")  # as hazard_late
      add_test(NAME ${t}_late COMMAND ${t} late)
    endif()
    set_tests_properties(${t}_frozen ${t}_ring ${t}_slots ${t}_fork PROPERTIES TIMEOUT 900)
    if(WTC_RT_SANITIZE STREQUAL "thread")
      set_tests_properties(${t}_fork PROPERTIES ENVIRONMENT "TSAN_OPTIONS=die_after_fork=0")
    endif()
    # A thread past the slots: counted, or refused under WTC_CHECK.
    if(WTC_CHECK)
      add_test(NAME ${t}_over_is_caught
               COMMAND sh -c "$<TARGET_FILE:${t}> over 2>&1 | grep -q 'more threads than it was built for'")
    else()
      add_test(NAME ${t}_over COMMAND ${t} over)
    endif()
  endforeach()
  add_executable(wcq_no_cas2 wcq_test.cpp)
  target_link_libraries(wcq_no_cas2 PRIVATE wtc_rt_flags)
  target_compile_definitions(wcq_no_cas2 PRIVATE WTC_TEST_EXPECT_NO_CAS2=1)
  add_test(NAME wcq_no_cas2_detected COMMAND wcq_no_cas2 cas2)
endif()

# Hazard records under thread churn: ten thousand threads, sixteen at a
# time, each pushing and popping; the records are reused, and every
# element arrives once. And a reader held on a segment while fifty are
# retired past it: it keeps its segment, and no more than its slots' worth.
add_executable(hazard_test hazard_test.cpp)
target_link_libraries(hazard_test PRIVATE wtc_rt_flags)
add_test(NAME hazard_churn COMMAND hazard_test churn 10000)
add_test(NAME hazard_fork COMMAND hazard_test fork)
# ThreadSanitizer tears its thread state down before glibc's later
# key-destructor rounds, and faults on any access from them.
if(NOT WTC_RT_SANITIZE STREQUAL "thread")
  add_test(NAME hazard_late COMMAND hazard_test late)
  add_test(NAME hazard_late_two COMMAND hazard_test late_two)
endif()
if(WTC_RT_SANITIZE STREQUAL "thread")
  set_tests_properties(hazard_fork PROPERTIES ENVIRONMENT "TSAN_OPTIONS=die_after_fork=0")
endif()
add_test(NAME hazard_stalled COMMAND hazard_test stalled)
add_test(NAME hazard_tail COMMAND hazard_test tail)
add_test(NAME hazard_nested COMMAND hazard_test nested)
# Four nested pushes must work and the fifth abort.
add_test(NAME hazard_deep_is_caught
         COMMAND sh -c "o=$($<TARGET_FILE:hazard_test> deep 2>&1); echo \"$o\" | grep -q 'four nested pushes done' && echo \"$o\" | grep -q 'a push nested more than 4 deep'")
add_test(NAME hazard_producers COMMAND hazard_test producers 4 100000)
set_tests_properties(hazard_churn hazard_producers PROPERTIES TIMEOUT 600)
# The same with a scan at every retire and freed memory overwritten, so
# that a segment freed while a push can still reach it shows; that with
# the one-counter push (jiffy); and that with no try at protecting the
# tail, so that every one-counter push holds every segment (all).
foreach(v eager jiffy all)
  add_executable(hazard_${v}_test hazard_test.cpp)
  target_link_libraries(hazard_${v}_test PRIVATE wtc_rt_flags)
  target_compile_definitions(hazard_${v}_test PRIVATE WTC_HAZARD_SCAN_AT=1)
  if(NOT WTC_RT_SANITIZE)
    target_compile_definitions(hazard_${v}_test PRIVATE WTC_TEST_POISON_FREED=1)
  endif()
  add_test(NAME hazard_${v}_tail COMMAND hazard_${v}_test tail)
  add_test(NAME hazard_${v}_nested COMMAND hazard_${v}_test nested)
  add_test(NAME hazard_${v}_churn COMMAND hazard_${v}_test churn 2000)
  add_test(NAME hazard_${v}_producers COMMAND hazard_${v}_test producers 4 100000)
  set_tests_properties(hazard_${v}_churn hazard_${v}_producers PROPERTIES TIMEOUT 600)
endforeach()
target_compile_definitions(hazard_jiffy_test PRIVATE WTC_TEST_ONE_COUNTER=1)
target_compile_definitions(hazard_all_test PRIVATE WTC_TEST_PROTECT_TRIES=0 WTC_TEST_ONE_COUNTER=1)
# A push nested in an element's move, with freed memory zeroed: a push
# that went on through a segment freed under it would lose elements.
add_executable(hazard_zero_test hazard_test.cpp)
target_link_libraries(hazard_zero_test PRIVATE wtc_rt_flags)
target_compile_definitions(hazard_zero_test PRIVATE WTC_HAZARD_SCAN_AT=1)
if(NOT WTC_RT_SANITIZE)
  target_compile_definitions(hazard_zero_test PRIVATE WTC_TEST_POISON_FREED=1 WTC_TEST_POISON_BYTE=0)
endif()
add_test(NAME hazard_zero_nested COMMAND hazard_zero_test nested)
add_test(NAME hazard_all_pin COMMAND hazard_all_test pin)
add_test(NAME hazard_all_relay COMMAND hazard_all_test relay)
if(NOT WTC_RT_SANITIZE)  # timed, and needs the threads to overlap
  add_test(NAME hazard_all_overlap COMMAND hazard_all_test overlap 8 2)
  set_tests_properties(hazard_all_overlap PROPERTIES RUN_SERIAL TRUE)
endif()
