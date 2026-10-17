#!/usr/bin/env python3
"""A random sample of census sites, for reading by hand, and the precision
that reading gives.

The census matches spellings. Whether a match is a decision about two
threads -- as opposed to a DMA buffer, a firmware layout, a SIMD table, a
test fixture -- only a reader can say, and a count published without that
reading is a count of spellings. So:

  corpus-sample.py draw  sites.csv N SEED name=root ...   > sample.csv
      N rows drawn at random from the mechanism sites (seeded, so the
      draw is reproducible), each printed with its source context so the
      reader can label it. The CSV has an empty `label` column.

  corpus-sample.py score sample.csv
      The labelled sample: precision as the share of `yes`, with a 95%
      Wilson interval, and the counts per label.

Labels: yes (separates data different threads touch), no (anything else),
unsure. The published number is the `yes` share with its interval; the
sample file is committed next to the document that cites it.
"""
import csv, math, random, sys
from pathlib import Path

CONTEXT = 3


def draw(sites, n, seed, roots):
    rows = [r for r in csv.DictReader(open(sites, newline=''))
            if r['idiom'] not in ('prose', 'simd_const', 'cacheline_macro')]
    rng = random.Random(seed)
    picked = rng.sample(rows, min(n, len(rows)))
    w = csv.writer(sys.stdout, lineterminator='\n')
    w.writerow(['codebase', 'file', 'line', 'idiom', 'text', 'context', 'label'])
    for r in picked:
        ctx = ''
        root = roots.get(r['codebase'])
        if root:
            try:
                lines = Path(root, r['file']).read_text(errors='replace').splitlines()
                ln = int(r['line'])
                lo, hi = max(1, ln - CONTEXT), min(len(lines), ln + CONTEXT)
                ctx = '\n'.join(f'{i:5d}{">" if i == ln else " "} {lines[i - 1]}'
                                for i in range(lo, hi + 1))
            except OSError:
                pass
        w.writerow([r['codebase'], r['file'], r['line'], r['idiom'], r['text'], ctx, ''])


def wilson(k, n, z=1.96):
    if n == 0:
        return 0.0, 0.0
    p = k / n
    d = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / d
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return centre - half, centre + half


def score(sample):
    rows = list(csv.DictReader(open(sample, newline='')))
    labels = [r['label'].strip().lower() for r in rows]
    n = len(labels)
    yes = labels.count('yes')
    unl = labels.count('')
    if unl:
        print(f'{unl} of {n} rows are not labelled yet', file=sys.stderr)
    for lab in ('yes', 'no', 'unsure'):
        print(f'{lab:<7}{labels.count(lab):>5}')
    lo, hi = wilson(yes, n - unl)
    print(f'\nprecision: {yes} of {n - unl} = {100 * yes / max(1, n - unl):.0f}% '
          f'(95% Wilson interval {100 * lo:.0f}%..{100 * hi:.0f}%)')
    # Per idiom for a census sample, per codebase for a recall sample.
    key = 'idiom' if 'idiom' in rows[0] else 'codebase'
    by_idiom = {}
    for r, lab in zip(rows, labels):
        by_idiom.setdefault(r[key], []).append(lab)
    print(f'\nby {key} (yes/total):')
    for idiom, labs in sorted(by_idiom.items(), key=lambda kv: -len(kv[1])):
        print(f'  {idiom:<18}{labs.count("yes"):>4}/{len(labs):<4}')


def main():
    if len(sys.argv) >= 5 and sys.argv[1] == 'draw':
        roots = dict(pair.split('=', 1) for pair in sys.argv[5:])
        draw(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), roots)
    elif len(sys.argv) == 3 and sys.argv[1] == 'score':
        score(sys.argv[2])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
