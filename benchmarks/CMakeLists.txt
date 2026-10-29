# Standalone: needs no LLVM. Uses the system compiler; the pinned ClangIR
# clang has no sanitizer runtime and is not what these binaries are about.
#
#   cmake -S benchmarks -B build-bench -DCMAKE_CXX_COMPILER=clang++-18
#   cmake --build build-bench -j8
#
# Options:
#   -DWTC_BENCH_SANITIZE=thread   ThreadSanitizer build (correctness legs)
#   -DWTC_CHECK=ON                 the shim's contract checks (negative control)
#   -DWTC_BENCH_MARCH=native       instruction set (recorded in meta.json)
#
# Never build inside a `bench` reservation; see benchmarks/README.md.
cmake_minimum_required(VERSION 3.20)
project(wtc-benchmarks C CXX)

# 20 for the LPRQ artifact's queues (concepts); everything else is 17-clean.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(WTC_BENCH_SANITIZE "" CACHE STRING "Sanitizer to enable (thread or empty)")
set(WTC_BENCH_MARCH "native" CACHE STRING "-march value")
option(WTC_CHECK "Build the shim and runtime with their contract checks" OFF)

find_package(Threads REQUIRED)
find_package(Boost 1.70 REQUIRED)
# Optional on purpose: oneTBB is a system package, and a machine without it
# should still be able to build and run everything else.
find_package(TBB QUIET)
if(TBB_FOUND)
  message(STATUS "oneTBB found: the onetbb_cq_sp baseline is available")
else()
  message(STATUS "oneTBB not found: building without the onetbb_cq_sp baseline")
endif()

set(WTC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_library(wtc_bench_flags INTERFACE)
target_include_directories(wtc_bench_flags INTERFACE
  ${WTC_ROOT}/shim
  ${WTC_ROOT}/runtime
  ${CMAKE_CURRENT_SOURCE_DIR}/queues
  ${CMAKE_CURRENT_SOURCE_DIR}/harness
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/readerwriterqueue
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/concurrentqueue
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rigtorp)
# Vendored verbatim and not warning-clean under -Wextra: a system include.
target_include_directories(wtc_bench_flags SYSTEM INTERFACE
  ${Boost_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lprq
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lprq/scqueue)
target_compile_options(wtc_bench_flags INTERFACE -Wall -Wextra -Werror -march=${WTC_BENCH_MARCH})
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # rigtorp uses std::hardware_destructive_interference_size; GCC warns that
  # its value is ABI-fragile. It is what we want here. -Wno-attributes: the
  # shim's [[clang::annotate]] is meaningless to GCC, as in the CI shim job.
  target_compile_options(wtc_bench_flags INTERFACE -Wno-interference-size -Wno-attributes)
endif()
if(WTC_BENCH_SANITIZE)
  target_compile_options(wtc_bench_flags INTERFACE -fsanitize=${WTC_BENCH_SANITIZE} -O1 -g)
  target_link_options(wtc_bench_flags INTERFACE -fsanitize=${WTC_BENCH_SANITIZE})
else()
  target_compile_options(wtc_bench_flags INTERFACE -O2)
endif()
if(WTC_CHECK)
  target_compile_definitions(wtc_bench_flags INTERFACE WTC_CHECK=1)
endif()
# The SCQ ring LSCQ is built on, C11 atomics, from the LPRQ artifact.
add_library(wtc_lfring STATIC third_party/lprq/scqueue/lfring_cas1.c)
target_include_directories(wtc_lfring PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lprq/scqueue)
target_compile_options(wtc_lfring PRIVATE -O2 -march=${WTC_BENCH_MARCH})
if(WTC_BENCH_SANITIZE)
  target_compile_options(wtc_lfring PRIVATE -fsanitize=${WTC_BENCH_SANITIZE} -g)
endif()
target_link_libraries(wtc_bench_flags INTERFACE Threads::Threads wtc_lfring)
if(TBB_FOUND)
  target_compile_definitions(wtc_bench_flags INTERFACE WTC_HAVE_TBB=1)
  target_link_libraries(wtc_bench_flags INTERFACE TBB::tbb)
endif()

foreach(w w1_throughput w2_latency w3_pipeline w5_mpmc_control w6_ring_shape w9_mpmc_scaling w10_ablation
          w11_pool w12_fusion)
  add_executable(${w} workloads/${w}.cpp)
  target_link_libraries(${w} PRIVATE wtc_bench_flags)
endforeach()

# The allocation counter: a preload for the runs that explain a result
# (harness/alloccount.cpp). Linked into nothing, and never sanitized: it
# replaces the allocator a sanitizer would also replace.
# W13 measures the laned queue, which the shim carries only in a build that
# asks for it (shim/topology.hpp).
add_executable(w13_mpsc_order workloads/w13_mpsc_order.cpp)
target_link_libraries(w13_mpsc_order PRIVATE wtc_bench_flags)
target_compile_definitions(w13_mpsc_order PRIVATE WTC_LANED_MPSC=1)

add_library(wtc_alloccount SHARED harness/alloccount.cpp)
target_compile_options(wtc_alloccount PRIVATE -O2 -Wall -Wextra -Werror -fno-builtin)

# What was built, for meta.json.
file(WRITE ${CMAKE_BINARY_DIR}/build-info.txt
  "compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
  "build_type=${CMAKE_BUILD_TYPE}\n"
  "march=${WTC_BENCH_MARCH}\n"
  "sanitize=${WTC_BENCH_SANITIZE}\n"
  "check=${WTC_CHECK}\n"
  "tbb=${TBB_FOUND}\n")
