# -*- Python -*-

import os
import platform
import shutil
import subprocess

import lit.formats

from lit.llvm import llvm_config

# Configuration file for the 'lit' test runner.

config.name = "WTC"

config.test_format = lit.formats.ShTest()

# suffixes: A list of file extensions to treat as test files.
config.suffixes = [".mlir", ".cpp"]

# test_source_root: The root path where tests are located.
config.test_source_root = os.path.dirname(__file__)

# test_exec_root: The root path where tests should be run.
config.test_exec_root = os.path.join(config.wtc_obj_root, "test")

config.substitutions.append(("%PATH%", config.environment["PATH"]))
config.substitutions.append(("%shlibext", config.llvm_shlib_ext))

llvm_config.with_system_environment(["HOME", "INCLUDE", "LIB", "TMP", "TEMP"])

llvm_config.use_default_substitutions()

# excludes: A list of directories/files to exclude from the testsuite.
config.excludes = ["Inputs", "CMakeLists.txt", "lit.cfg.py", "lit.site.cfg.py.in"]

config.wtc_tools_dir = os.path.join(config.wtc_obj_root, "bin")

# Tweak the PATH to include the tools dir.
llvm_config.with_environment("PATH", config.llvm_tools_dir, append_path=True)

tool_dirs = [config.wtc_tools_dir, config.llvm_tools_dir]
tools = [
    "wtc-opt",
]

llvm_config.add_tool_substitutions(tools, tool_dirs)

# Tests that start from C++ need the pinned clang, with ClangIR, and only
# that one: a clang from PATH would compile them with a different frontend
# from the one the passes are built against. %cir_cxx emits CIR for a
# program written against the shim.
clang = shutil.which("clang", path=config.llvm_tools_dir)
if clang:
    cir_cxx = [
        clang, "-x", "c++", "-std=c++17", "-fclangir", "-emit-cir",
        "-I", os.path.join(config.wtc_src_root, "shim"),
    ]
    # A clang built from source does not know where macOS keeps the SDK.
    if platform.system() == "Darwin":
        sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
        cir_cxx += ["-isysroot", sdk]
    config.substitutions.append(("%cir_cxx", " ".join(cir_cxx)))
    config.available_features.add("clangir")

    # Whether a value comes back through a hidden argument depends on the
    # target: on x86-64 a non-trivial record does, on AArch64 it does not,
    # and the passes have to handle both. %cir_x86_64_cxx emits for x86-64
    # wherever the host's headers allow it, so both shapes are tested on
    # either kind of machine.
    cir_cxx_x86_64 = None
    if platform.machine() in ("x86_64", "AMD64"):
        cir_cxx_x86_64 = cir_cxx
    elif platform.system() == "Darwin":
        cir_cxx_x86_64 = cir_cxx + ["-target", "x86_64-apple-macos14"]
    if cir_cxx_x86_64:
        config.substitutions.append(("%cir_x86_64_cxx", " ".join(cir_cxx_x86_64)))
        config.available_features.add("clangir-x86-64")

# cir-opt takes a test on from CIR to the LLVM dialect, which is where a
# substitution that produced inconsistent IR would fail.
if shutil.which("cir-opt", path=config.llvm_tools_dir):
    llvm_config.add_tool_substitutions(["cir-opt"], [config.llvm_tools_dir])
    config.available_features.add("cir-opt")
