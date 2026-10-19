# -*- Python -*-

import os
import subprocess
import tempfile

import lit.formats

from lit.llvm import llvm_config

# Configuration file for the 'lit' test runner.

config.name = "WTC"

config.test_format = lit.formats.ShTest()

# .mlir tests drive wtc-opt on hand-written IR. .cpp tests drive the whole
# pipeline from source, which is the only kind of test that can catch a
# mismatch between what the compiler emits and what the passes expect.
config.suffixes = [".mlir", ".cpp", ".c"]

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
    "cir-translate",
    "not",
    "count",
]
llvm_config.add_tool_substitutions(tools, tool_dirs)

# The pinned ClangIR clang, from the same tools directory as cir-translate.
clangxx = os.path.join(config.llvm_tools_dir, "clang++")
config.substitutions.append(("%clangxx", clangxx))
# %clang for C sources: the C++ driver rejects -std=gnu11 outright.
config.substitutions.append(("%clang", os.path.join(config.llvm_tools_dir, "clang")))
# wtc-cc takes clang++ and cir-translate from the toolchain (the parent of
# its bin/) and wtc-opt from the WTC_OPT environment variable set below.
config.substitutions.append(("%wtc_cc", os.path.join(config.wtc_src_root, "tools", "wtc-cc")
                             + " --toolchain " + os.path.dirname(config.llvm_tools_dir)))
config.substitutions.append(("%shim", os.path.join(config.wtc_src_root, "shim")))
config.substitutions.append(("%examples", os.path.join(config.wtc_src_root, "examples")))
config.substitutions.append(("%wtc_src", config.wtc_src_root))

# wtc-cc looks for wtc-opt in the build tree; tell it where this build is.
config.environment["WTC_OPT"] = os.path.join(config.wtc_tools_dir, "wtc-opt")

# The pinned clang carries no sanitizer runtime, so ThreadSanitizer tests use
# the system compiler and run only where it works.
def _tsan_works():
    for cc in ("clang++-18", "clang++", "g++"):
        try:
            with tempfile.TemporaryDirectory() as d:
                src = os.path.join(d, "t.cpp")
                with open(src, "w") as f:
                    f.write("int main(){}\n")
                exe = os.path.join(d, "t")
                if subprocess.run([cc, "-fsanitize=thread", src, "-o", exe],
                                  capture_output=True).returncode != 0:
                    continue
                if subprocess.run([exe], capture_output=True).returncode == 0:
                    config.substitutions.append(("%tsan_cxx", cc))
                    # wtc-cc --tsan links with it too.
                    config.environment["WTC_TSAN_CXX"] = cc
                    return True
        except OSError:
            continue
    return False

if _tsan_works():
    config.available_features.add("tsan")
