#!/usr/bin/env python3
"""Paired A/B of the ring's latency gate: old build against new, per round.

Usage: ab.py runs.jsonl
Each record's "alloc" field carries the side ("old" or "new"); both sides
ran the same configuration in the same round. Ratios are new/old within a
round, the median per configuration, then the geometric mean over
configurations with the range of the medians.
"""
import json, math, statistics as st, sys
from collections import defaultdict

LATE_NS = 896          # histogram bucket bound nearest 0.9 us

def late_share(r):
    h = r.get("hist_ns")
    if not h:
        return None
    tot = late = 0
    for kv in h.split(","):
        b, n = kv.split(":")
        n = int(n); tot += n
        if int(b) >= LATE_NS:
            late += n
    return late / tot if tot else None

def gm(xs):
    return math.exp(sum(map(math.log, xs)) / len(xs))

rows = [json.loads(l) for l in open(sys.argv[1])]
cell = defaultdict(dict)          # (workload, variant, param, rep) -> side -> record
for r in rows:
    w = r["workload"]
    p = {"W1": r.get("elem"), "W2": r.get("interval_ns"), "W3": r.get("rate")}[w]
    cell[(w, r["variant"], p, r["rep"])][r["alloc"]] = r

def table(w, metric, better, fmt="{:.3g}"):
    conf = defaultdict(lambda: {"old": [], "new": [], "ratio": []})
    for (ww, v, p, rep), s in cell.items():
        if ww != w or "old" not in s or "new" not in s:
            continue
        a, b = metric(s["old"]), metric(s["new"])
        if a is None or b is None:
            continue
        c = conf[(v, p)]
        c["old"].append(a); c["new"].append(b)
        if a > 0 and b > 0:
            c["ratio"].append(b / a)
    out = {}
    for (v, p), c in sorted(conf.items(), key=lambda kv: (kv[0][0], float(kv[0][1]))):
        med = st.median(c["ratio"]) if c["ratio"] else float("nan")
        out[(v, p)] = med
        print(f"  {w} {v:18s} {str(p):>8s}  old {fmt.format(st.median(c['old'])):>8s}"
              f"  new {fmt.format(st.median(c['new'])):>8s}  new/old {med:.3f}  n={len(c['ratio'])}")
    by_v = defaultdict(list)
    for (v, p), m in out.items():
        if m == m:
            by_v[v].append(m)
    for v, ms in sorted(by_v.items()):
        print(f"  {w} {v:18s} geomean new/old {gm(ms):.3f} ({min(ms):.3f}-{max(ms):.3f}) [{better}]")
    return out

print("W1 throughput, M elements/s (higher is better)")
table("W1", lambda r: r["mops"], "higher better")
print("W2 latency p50 ns by interval (lower is better)")
table("W2", lambda r: r["p50"], "lower better")
print("W2 latency p99 ns by interval")
table("W2", lambda r: r["p99"], "lower better")
print("W3 pipeline p50 ns by rate")
table("W3", lambda r: r["p50"], "lower better")
print("W3 pipeline p99 ns by rate")
table("W3", lambda r: r["p99"], "lower better")
print("W3 share of elements at or above 0.9 us (lower is better)")
table("W3", late_share, "lower better", "{:.4f}")
print("W3 saturated runs (old/new):",
      sum(1 for r in rows if r["workload"] == "W3" and r["alloc"] == "old" and r.get("saturated")),
      sum(1 for r in rows if r["workload"] == "W3" and r["alloc"] == "new" and r.get("saturated")))
