#!/usr/bin/env python3
"""What the census may have missed: lines that talk about cache lines in
code and are not census sites.

A census built from a list of spellings finds the spellings on its list.
This scans the same trees with one broad pattern -- any identifier naming a
cache line, an interference size or a padded type, on a code line -- and
prints the lines the census did not record, with context, as a CSV the
reader labels the same way as corpus-sample.py's draw. The share labelled
`yes` is the census's miss rate on that tree, which is the number the
document owes next to its site count.

  corpus-recall.py sites.csv name=root ... > candidates.csv
"""
import csv, os, re, sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib import import_module
census = import_module('corpus-census')

BROAD = re.compile(r'(?i)cache_?line|cacheline|interference_size|nfs_size|cl_pad|'
                   r'[a-z_]*padded<|\bpad\w*\s*\[')
CONTEXT = 2


def main():
    sites = sys.argv[1]
    roots = dict(pair.split('=', 1) for pair in sys.argv[2:])
    known = {(r['codebase'], r['file'], int(r['line']))
             for r in csv.DictReader(open(sites, newline=''))}
    w = csv.writer(sys.stdout, lineterminator='\n')
    w.writerow(['codebase', 'file', 'line', 'text', 'context', 'label'])
    for name, root in roots.items():
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames
                           if d not in census.SKIP and not d.startswith(census.SKIP_PREFIX)
                           and d not in census.TEST_DIRS]
            for fn in filenames:
                if not fn.endswith(census.SRC) or census.TEST_FILE.search(fn):
                    continue
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, root)
                try:
                    lines = Path(path).read_text(errors='replace').splitlines()
                except OSError:
                    continue
                in_comment = False
                for i, line in enumerate(lines, 1):
                    code, in_comment = census.strip_comments(line, in_comment)
                    if census.MACRO_DEF.match(line) or not BROAD.search(code):
                        continue
                    if (name, rel, i) in known:
                        continue
                    lo, hi = max(1, i - CONTEXT), min(len(lines), i + CONTEXT)
                    ctx = '\n'.join(f'{j:5d}{">" if j == i else " "} {lines[j - 1]}'
                                    for j in range(lo, hi + 1))
                    w.writerow([name, rel, i, line.strip()[:160], ctx, ''])


if __name__ == '__main__':
    main()
