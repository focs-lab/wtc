#!/usr/bin/env python3
"""How real programs synchronise, counted: the census behind the
evaluation's question of reach (RQ1).

WTC recognises a queue or a mutex only where a program uses the shim, so on
unported code the number of objects it can specialise is zero by
construction. What this counts is the size of what it could reach: the
mutexes, condition variables and thread starts real programs declare, and
the queues they use, library by library -- the targets of the library
models in the evaluation plan, and the reason they come before anything
else there.

    corpus-sync-census.py name=path [name=path ...] [--detail sites.csv]

Prints one Markdown row per tree: files read, files set aside as tests,
and the count of each idiom on code lines (comments and string literals
removed, as the cache-line census does). A line counts once per idiom.
The walk, the skipped directories and the test filter are the cache-line
census's (corpus-census.py), so the two censuses read the same files.
"""
import argparse
import csv
import importlib.util
import os
import re
import sys
from collections import Counter

_spec = importlib.util.spec_from_file_location(
    "corpus_census", os.path.join(os.path.dirname(os.path.abspath(__file__)), "corpus-census.py"))
census = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(census)

# Declarations and calls, spelled the way the libraries and the C library
# spell them. A count is of lines, not of objects: an array of mutexes is
# one line, and a mutex declared in a struct counts where it is declared.
IDIOMS = {
    # mutual exclusion
    "pthread_mutex":   re.compile(r"\bpthread_mutex_t\b"),
    "pthread_spin":    re.compile(r"\bpthread_spinlock_t\b"),
    "std_mutex":       re.compile(r"\bstd::(?:mutex|timed_mutex|recursive_mutex|shared_mutex)\b"),
    # waiting
    "pthread_cond":    re.compile(r"\bpthread_cond_t\b"),
    "std_condvar":     re.compile(r"\bstd::condition_variable(?:_any)?\b"),
    # thread starts
    "pthread_create":  re.compile(r"\bpthread_create\s*\("),
    "std_thread":      re.compile(r"\bstd::(?:thread|jthread)\b"),
    # queues from libraries
    "tbb_queue":       re.compile(r"\btbb::concurrent_(?:bounded_)?queue\b"),
    "moodycamel":      re.compile(r"\bmoodycamel::(?:Blocking)?(?:ConcurrentQueue|ReaderWriterQueue)\b"),
    "folly_queue":     re.compile(r"\bfolly::(?:ProducerConsumerQueue|MPMCQueue|UnboundedQueue|"
                                  r"U(?:SP|MP)(?:SC|MC)Queue|DMPSCQueue|DMPMCQueue)\b"),
    "boost_lockfree":  re.compile(r"\bboost::lockfree::(?:queue|spsc_queue)\b"),
    "dpdk_ring":       re.compile(r"\brte_ring_create(?:_elem)?\s*\("),
    "ck_ring":         re.compile(r"\bck_ring_(?:init|enqueue_spsc|enqueue_mpmc)\b"),
}


def scan(root):
    counts, sites = Counter(), []
    nfiles = ntests = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in census.SKIP and not d.startswith(census.SKIP_PREFIX)]
        in_tests = any(census.is_test_dir(part) for part in os.path.relpath(dirpath, root).split(os.sep))
        for fn in filenames:
            if not fn.endswith(census.SRC):
                continue
            if in_tests or census.TEST_FILE.search(fn):
                ntests += 1
                continue
            path = os.path.join(dirpath, fn)
            nfiles += 1
            try:
                with open(path, "r", errors="replace") as f:
                    in_comment = False
                    for lineno, line in enumerate(f, 1):
                        code, in_comment = census.strip_comments(line, in_comment)
                        for name, rx in IDIOMS.items():
                            if rx.search(code):
                                counts[name] += 1
                                sites.append((os.path.relpath(path, root), lineno, name, line.strip()[:160]))
            except OSError:
                pass
    return nfiles, ntests, counts, sites


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trees", nargs="+", help="name=path")
    ap.add_argument("--detail", help="write every site to this CSV")
    a = ap.parse_args()
    cols = list(IDIOMS)
    print("| tree | files | tests set aside | " + " | ".join(cols) + " |")
    print("|---|---|---|" + "---|" * len(cols))
    total, detail = Counter(), []
    for spec in a.trees:
        name, _, path = spec.partition("=")
        if not os.path.isdir(path):
            print(f"{path}: not a directory", file=sys.stderr)
            continue
        nfiles, ntests, counts, sites = scan(path)
        total.update(counts)
        print(f"| {name} | {nfiles} | {ntests} | " + " | ".join(str(counts[c]) for c in cols) + " |")
        detail += [(name,) + s for s in sites]
    print("| **all** | | | " + " | ".join(str(total[c]) for c in cols) + " |")
    if a.detail:
        with open(a.detail, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["tree", "file", "line", "idiom", "text"])
            w.writerows(detail)


if __name__ == "__main__":
    main()
