#!/usr/bin/env python3
"""Step A of the WTC corpus census.

Count, and locate, the cache-line annotations the authors of real programs
wrote by hand.  Each one marks a place where a human expert already decided
"different threads touch these", so the set of them is the oracle the
ownership analysis is measured against: precision is how often the analysis
agrees, recall is how much of it the analysis finds.

Emits a summary table on stdout and, with --detail FILE, one CSV row per
site so a later run can be compared against it line by line.
"""
import argparse, csv, os, re, sys
from collections import Counter

SRC = ('.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.hh')
# A checked-in build tree is not source. Every one of LLVM's 34 reported
# sites turned out to be lit-test output under cmake-build-release, which
# this set did not name.
SKIP = {'.git', 'build', 'out', 'third_party', 'node_modules', '.github',
        '_build', 'Output'}
SKIP_PREFIX = ('cmake-build', 'build-', 'out-')
# Tests are not the program: a fixture aligned to 64 to exercise an
# allocator is not a decision about two threads, and a sample of the
# census's hits found compiler-rt's new_delete_test and a protobuf test
# among them. Skipped, and counted, so the share set aside is known.
# Any directory whose name starts with `test` (DPDK's app/test-flow-perf and
# its siblings are test applications), plus the usual spellings, plus
# benchmarks, which are no more the program than tests are.
TEST_DIRS = {'unittest', 'unittests', 'testsuite', 'gtest', 'benchmarks', 'bench'}
def is_test_dir(name):
    return name.lower().startswith('test') or name in TEST_DIRS
TEST_FILE = re.compile(r'(?:^test_\w*|\w*_(?:test|tests|unittest|bench))\.(?:c|cc|cpp|cxx|h|hpp|hh)$'
                       r'|Test\.(?:cc|cpp|cxx|h|hpp|hh)$')

# A site is a mechanism (the compiler is told) or prose (only a human is told).
MECHANISM = {
    'alignas_literal':   re.compile(r'alignas\s*\(\s*(?:64|128|256)\s*\)'),
    'alignas_interf':    re.compile(r'alignas\s*\([^)]*hardware_(?:destructive|constructive)_interference_size'),
    # Names are matched without regard to case: folly writes
    # alignas(cacheline_align_v) and protobuf alignas(kCacheAlignment), and
    # a case-sensitive list missed about a quarter of folly's sites.
    'alignas_named':     re.compile(r'alignas\s*\([^)]*(?i:cache|cacheline|cache_line|level1)'),
    'attr_aligned':      re.compile(r'__attribute__\s*\(\(\s*(?:__)?aligned(?:__)?\s*\(\s*(?:64|128|256|[A-Za-z_]*(?i:cache|level1)[A-Za-z_]*)\s*\)'),
    'kernel_aligned':    re.compile(r'_{2,4}cacheline_aligned(?:_in_smp)?'),
    'kernel_internode':  re.compile(r'_{2,4}cacheline_internodealigned_in_smp'),
    # include/linux/cache.h:107-147 -- the kernel's dedicated way of saying
    # "these members are one group, that line is the boundary". A group is
    # opened and closed, and is one decision: only the opening is counted,
    # or every group would count twice. Only the _aligned form: the plain
    # __cacheline_group_begin is a zero-length marker that aligns nothing
    # (cache.h:107-110), so it tells the compiler nothing.
    'kernel_group':      re.compile(r'__cacheline_group_begin_aligned\s*\('),
    'kernel_aligned_to': re.compile(r'__aligned\s*\(\s*(?:SMP_CACHE_BYTES|L1_CACHE_BYTES)\s*\)'),
    'kernel_padding':    re.compile(r'\bCACHELINE_PADDING\s*\('),
    'dpdk_guard':        re.compile(r'\bRTE_CACHE_GUARD\b'),
    'tbb_nfs':           re.compile(r'alignas\s*\(\s*max_nfs_size\s*\)'),
    # Project macros that spell an alignment to a cache line: rocksdb's
    # ALIGN_AS(CACHE_LINE_SIZE), jemalloc's JEMALLOC_ALIGNED(CACHELINE),
    # abseil's ABSL_CACHELINE_ALIGNED, folly's cacheline_aligned<T>.
    # compiler-rt's ALIGNED(SANITIZER_CACHE_LINE_SIZE), MySQL's
    # ut::Cacheline_padded<T> and Cacheline_aligned<T>, folly's
    # CachelinePadded<T>.
    'project_aligned':   re.compile(r'\b(?:ALIGN_AS|JEMALLOC_ALIGNED|ALIGNED)\s*\(\s*\w*(?i:CACHE_?LINE)\w*\s*\)'
                                    r'|\bABSL_CACHELINE_ALIGNED\b'
                                    r'|\b\w*(?i:cacheline_?(?:aligned|padded))\w*\s*<'),
    'dpdk_aligned':      re.compile(r'__rte_cache_(?:aligned|min_aligned)'),
    'declspec_align':    re.compile(r'__declspec\s*\(\s*align\s*\(\s*(?:64|128)'),
    # Padding alone means nothing: most `uint8_t pad[7]` in real code is
    # hardware register, wire or on-disk format, not a thread decision.
    # Sampled hits included an AMD GPU firmware struct, a DPDK command
    # descriptor, a MIPS MMIO window and the XFS log format -- none of them
    # ours.  Require either a cache-line-sized array or a name that says so.
    # Byte types, plus MySQL's `byte` and folly's `std::array<uint8_t,
    # hardware_destructive_interference_size> padding`. A version that took
    # any element type admitted 29 hardware and on-disk layouts (`u32
    # pad[64]`, `__u8 pad[...]`) for the 10 thread decisions it gained.
    'pad_field':         re.compile(r'^\s*(?:volatile\s+)?(?:char|unsigned char|uint8_t|u_int8_t|std::byte|byte)\s+'
                                    r'(?:_*(?:cacheline|cache_line|cl)_?pad\w*\s*\['
                                    r'|_*(?:pad|padding)\w*\s*\[\s*(?:64|128|256'
                                    r'|[^]]*(?i:cache|level1|nfs|line_?size|interference)))'
                                    r'|^\s*std::array\s*<[^>]*(?i:cache|interference)[^>]*>\s+\w*(?i:pad)'),
    'cacheline_macro':   re.compile(r'^\s*#\s*define\s+\w*CACHE_?LINE\w*'),
}
# Any preprocessor directive, tested before the mechanisms so that a macro
# defined *using* an aligned attribute cannot be counted as a use of one.
# Every directive, not only #define: `#if !defined(____cacheline_...)` and
# an #include whose comment names the macro were counted as sites, and the
# classifier, which sets aside every line starting with '#', then disagreed
# with this script's total. One rule for both.
MACRO_DEF = re.compile(r'^\s*#')
CACHE_NAME = re.compile(r'CACHE_?LINE|cacheline', re.I)
PROSE = re.compile(r'(?:cache.?line|false.?shar)', re.I)
# An alignment annotation means false sharing only on data somebody writes.
# A `static const` table aligned to 64 is a vector-load alignment for SIMD,
# which is a different problem with the same spelling: every one of FFmpeg's
# nine hits was `static const int16_t ... __attribute__((aligned (64)))` in
# hand-written MIPS kernels.  Counting those as oracle sites would wreck
# precision before the analysis has said anything.
IMMUTABLE = re.compile(r'\bconst\b|\bconstexpr\b')
CODEISH = re.compile(r'^\s*(?://|/\*|\*|#\s*(?!define))')

def strip_comments(line, in_comment):
    """The line with comments and string literals removed, and whether a
    block comment is still open at its end. A literal is dropped whole: six
    `printf("... __attribute__((aligned(256))) ...")` lines in a kernel
    table generator were once counted as sites."""
    out, i, n = [], 0, len(line)
    while i < n:
        if in_comment:
            j = line.find('*/', i)
            if j < 0:
                return ''.join(out), True
            i, in_comment = j + 2, False
            continue
        if line.startswith('//', i):
            break
        if line.startswith('/*', i):
            in_comment, i = True, i + 2
            continue
        c = line[i]
        if c in '"\'':
            i += 1
            while i < n and line[i] != c:
                i += 2 if line[i] == '\\' else 1
            i += 1
            out.append('""' if c == '"' else "' '")
            continue
        out.append(c); i += 1
    return ''.join(out), in_comment

def scan(root):
    counts, sites = Counter(), []
    nfiles = ntests = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames
                       if d not in SKIP and not d.startswith(SKIP_PREFIX)]
        in_tests = any(is_test_dir(part)
                       for part in os.path.relpath(dirpath, root).split(os.sep))
        for fn in filenames:
            if not fn.endswith(SRC):
                continue
            if in_tests or TEST_FILE.search(fn):
                ntests += 1
                continue
            path = os.path.join(dirpath, fn)
            nfiles += 1
            try:
                with open(path, 'r', errors='replace') as f:
                    in_macro = False
                    in_comment = False
                    for lineno, line in enumerate(f, 1):
                        hit = None
                        # The code on this line, comments removed. A
                        # mechanism named in a comment -- cache.h explaining
                        # its own macros, a header saying what it does not
                        # do -- tells the compiler nothing.
                        code, in_comment = strip_comments(line, in_comment)
                        # A directive, or a line continuing one, is
                        # infrastructure. It is recorded only when it is
                        # about cache lines, so that the classifier can count
                        # what was set aside; every other directive is
                        # skipped silently.
                        directive = in_macro or MACRO_DEF.match(line)
                        in_macro = bool(directive) and line.rstrip().endswith('\\')
                        if directive:
                            if any(rx.search(line) for rx in MECHANISM.values()) \
                                    or CACHE_NAME.search(line):
                                counts['cacheline_macro'] += 1
                                sites.append((os.path.relpath(path, root), lineno,
                                              'cacheline_macro', line.strip()[:160]))
                            continue
                        for name, rx in MECHANISM.items():
                            if rx.search(code):
                                hit = ('simd_const'
                                       if name != 'cacheline_macro'
                                       and IMMUTABLE.search(code)
                                       else name)
                                break
                        if hit is None and PROSE.search(line) and CODEISH.match(line):
                            hit = 'prose'
                        if hit:
                            counts[hit] += 1
                            sites.append((os.path.relpath(path, root), lineno, hit,
                                          line.strip()[:160]))
            except OSError:
                pass
    return nfiles, ntests, counts, sites

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trees', nargs='+', help='name=path')
    ap.add_argument('--detail', help='write every site to this CSV')
    a = ap.parse_args()

    cols = list(MECHANISM) + ['simd_const', 'prose']
    rows, detail = [], []
    for spec in a.trees:
        name, _, path = spec.partition('=')
        if not os.path.isdir(path):
            print(f'{name:<12} MISSING {path}', file=sys.stderr)
            continue
        nfiles, ntests, counts, sites = scan(path)
        # A `#define CACHE_LINE_SIZE 64` is infrastructure, not a decision
        # about two threads: it says the author knows what a cache line is,
        # not that this struct was split because of one.  Only uses count.
        mech = sum(counts[c] for c in cols
                   if c not in ('prose', 'simd_const', 'cacheline_macro'))
        rows.append((name, nfiles, ntests, counts, mech))
        detail += [(name,) + s for s in sites]

    w = max((len(r[0]) for r in rows), default=8) + 1
    hdr = (f'{"codebase":<{w}}{"files":>7}{"tests":>7}' +
           ''.join(f'{c[:9]:>10}' for c in cols) + f'{"SITES":>7}')
    print(hdr); print('-' * len(hdr))
    for name, nfiles, ntests, counts, mech in sorted(rows, key=lambda r: -r[4]):
        print(f'{name:<{w}}{nfiles:>7}{ntests:>7}' +
              ''.join(f'{counts[c]:>10}' for c in cols) + f'{mech:>7}')
    print('-' * len(hdr))
    print(f'{"TOTAL":<{w}}{sum(r[1] for r in rows):>7}{sum(r[2] for r in rows):>7}' +
          ''.join(f'{sum(r[3][c] for r in rows):>10}' for c in cols) +
          f'{sum(r[4] for r in rows):>7}')
    print('\n"tests" counts source files under test directories or named *_test.*, '
          'which are skipped: a fixture is not a decision about two threads.')

    if a.detail:
        with open(a.detail, 'w', newline='') as f:
            wr = csv.writer(f)
            wr.writerow(['codebase', 'file', 'line', 'idiom', 'text'])
            wr.writerows(detail)
        print(f'\n{len(detail)} sites -> {a.detail}')

if __name__ == '__main__':
    main()
