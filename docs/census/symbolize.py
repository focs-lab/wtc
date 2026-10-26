#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Rewrites the frames of census reports that backtrace_symbols_fd printed
as "binary(+0xOFF)[0xADDR]" into "binary: function (file:line)", with
addr2line on the binary (built with -g). Frames in the system's libraries
keep their offsets: their symbols are not in the binary, and addr2line
would name the nearest exported function instead. Every frame then names
its binary by file name only, so a report says nothing of the directories
it was built in, and frames are cut at 160 characters (C++ names run to
thousands); the summary lines are left whole.

    docs/census/symbolize.py <report>...
"""
import re, subprocess, sys, os
pat = re.compile(r"^(\S[^(]*)\(\+(0x[0-9a-f]+)\)\[0x[0-9a-f]+\]$")
for path in sys.argv[1:]:
    lines = open(path).read().split("\n")
    want = {}
    for l in lines:
        m = pat.match(l)
        if m:
            b = m.group(1).split(" ")[0]
            if os.path.exists(b) and not b.startswith(("/lib", "/usr/lib")): want.setdefault(b, set()).add(m.group(2))
    names = {}
    for b, offs in want.items():
        offs = sorted(offs)
        # return addresses: look up the call instruction, one byte before
        q = ["%#x" % (int(o, 16) - 1) for o in offs]
        out = subprocess.run(["addr2line", "-f", "-C", "-e", b] + q, capture_output=True, text=True).stdout.split("\n")
        for i, o in enumerate(offs):
            fn, loc = out[2 * i], out[2 * i + 1]
            loc = re.sub(r" \(discriminator \d+\)", "", loc)
            names[(b, o)] = fn if loc.startswith("??") else "%s (%s)" % (fn, os.path.basename(loc))
    res = []
    for l in lines:
        m = pat.match(l)
        if m:
            b = m.group(1).split(" ")[0]
            n = names.get((b, m.group(2)))
            if n and not n.startswith("??"):
                l = "%s: %s" % (os.path.basename(b), n)
        # any other frame: "/dir/binary(...)" -> "binary(...)"
        l = re.sub(r"^/\S*/([^/(\s]+)", r"\1", l)
        frame = not l.startswith(("threadcensus:", "single-thread", "reference counts:", "top ", "0x")) and \
            not re.match(r"^\S+ on ", l)
        res.append(l if len(l) <= 160 or not frame else l[:157] + "...")
    open(path, "w").write("\n".join(res))
