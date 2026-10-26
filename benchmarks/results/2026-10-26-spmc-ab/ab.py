"""Paired ratios for the spmc A/B: python3 ab.py w11-runs.jsonl w9-runs.jsonl

Each run carries "build" (base: the ring whose consumers took turns; new:
the lock-free queue) and its repetition. A ratio is taken within a
repetition, its median over the repetitions per configuration, and the
geometric mean of those medians across configurations, with their range.
"""
import json, math, statistics as st, sys


def load(path, key):
    d = {}
    for line in open(path):
        r = json.loads(line)
        d[(r["build"] + "@" + r["variant"],) + key(r) + (r["rep"],)] = r["mops"]
    return d


def medians(d, a, b):
    out = {}
    for k in d:
        if k[0] != a:
            continue
        cfg = k[1:-1]
        if cfg in out:
            continue
        p = [d[(a,) + cfg + (r,)] / d[(b,) + cfg + (r,)] for r in range(100)
             if (a,) + cfg + (r,) in d and (b,) + cfg + (r,) in d]
        if p:
            out[cfg] = (st.median(p), min(p), max(p), len(p))
    return out


def gm(v):
    return math.exp(sum(map(math.log, v)) / len(v))


W11_PAIRS = [("new@cell_spmc", "base@cell_mpmc"), ("base@cell_spmc", "base@cell_mpmc"),
             ("new@cell_spmc", "base@cell_spmc"), ("new@heap_spmc", "base@heap_mpmc"),
             ("base@heap_spmc", "base@heap_mpmc"), ("new@heap_spmc", "base@heap_spmc")]
W9_PAIRS = [("new@shim_spmc", "base@shim_generic"), ("base@shim_spmc", "base@shim_generic"),
            ("new@shim_spmc", "base@shim_spmc")]

w11 = load(sys.argv[1], lambda r: (r["consumers"], r["work"], int(r["elem"])))
print("W11 (consumers, work ns, capture bytes): median paired ratio (range over repetitions)")
for a, b in W11_PAIRS:
    m = medians(w11, a, b)
    print(f"\n{a} / {b}: geometric mean {gm([x[0] for x in m.values()]):.2f} "
          f"(medians {min(x[0] for x in m.values()):.2f}-{max(x[0] for x in m.values()):.2f}), "
          f"{sum(1 for x in m.values() if x[0] < 0.95)} of {len(m)} configurations below 0.95")
    for cfg in sorted(m):
        x = m[cfg]
        print(f"  {cfg[0]} {cfg[1]:4} {cfg[2]:3}  {x[0]:.2f} ({x[1]:.2f}-{x[2]:.2f}) n={x[3]}")

w9 = load(sys.argv[2], lambda r: (r["consumers"],))
print("\nW9 split, one producer (consumers): median paired ratio (range)")
for a, b in W9_PAIRS:
    m = medians(w9, a, b)
    print(f"{a} / {b}: " + "; ".join(f"1:{c[0]} {x[0]:.2f} ({x[1]:.2f}-{x[2]:.2f})" for c, x in sorted(m.items())))
