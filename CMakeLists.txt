cmake_minimum_required(VERSION 3.20.0)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

project(wtc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find local LLVM/MLIR build.
find_package(LLVM REQUIRED CONFIG)
find_package(MLIR REQUIRED CONFIG)
# ClangIR lives in the Clang package; it exists only in a build configured
# with -DCLANG_ENABLE_CIR=ON. See README.md for the pinned revision.
find_package(Clang REQUIRED CONFIG)

# Now that the packages are found, MLIR_CMAKE_DIR and LLVM_CMAKE_DIR are defined.
# We can add them to the module path so that we can find AddMLIR.cmake, etc.
list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}/Testing")

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Using MLIRConfig.cmake in: ${MLIR_DIR}")
message(STATUS "Using ClangConfig.cmake in: ${Clang_DIR}")

include(TableGen)
include(AddLLVM)
include(AddMLIR)
include(HandleLLVMOptions)

# Default lit options, and put all our tools (wtc-opt, etc.) in a
# canonical build/bin so lit.cfg.py can find them via LLVM_TOOLS_DIR-style
# substitution, matching mlir/examples/standalone's layout.
set(LLVM_LIT_ARGS "-sv" CACHE STRING "Default options for lit")
set(LLVM_RUNTIME_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/bin)
set(LLVM_LIBRARY_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/lib)
set(MLIR_BINARY_DIR ${CMAKE_BINARY_DIR})

set(WTC_SOURCE_DIR ${PROJECT_SOURCE_DIR})
set(WTC_BINARY_DIR ${PROJECT_BINARY_DIR})

# Includes from the MLIR installation
include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${MLIR_INCLUDE_DIRS})

# Clang and ClangIR headers, including the generated ones
include_directories(${CLANG_INCLUDE_DIRS})

# Our own generated headers will land here
include_directories(${PROJECT_SOURCE_DIR}/mlir/include)
include_directories(${PROJECT_BINARY_DIR}/mlir/include)

add_definitions(${LLVM_DEFINITIONS})

add_subdirectory(mlir)
add_subdirectory(test)