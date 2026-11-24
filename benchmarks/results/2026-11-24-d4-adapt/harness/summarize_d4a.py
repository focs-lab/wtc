#!/usr/bin/env python3
# Copy of the script the leg ran, with its paths written as $LEGS (the legs' working
# directory), $PROBES (the probes' material), $WTC_D4A (the worktree at the measured
# commit), $LEG_DIR (the host's leg directory).
"""Summary of the adaptive delegation leg's runs.jsonl (leg_d4a.py).

    summarize_d4a.py <leg-dir> [--gate 0.10] [--csv summary.csv]

As the D4 leg's summarize.py: medians over the repetitions with a bootstrap 95%
interval of the median, min-max, the coefficient of variation (cells above 3% marked
*), ratios of medians, CPU time; the A/A check (orig-aa against orig); the controls
(median of three before and after, 5% threshold); a drift check (each configuration
normalised to its first repetition: the median over configurations of the last third
against the first third); the disturbance gate (runs whose processors outside the
leg's set were busy above the gate on average are retired and listed); and, for the
adaptive builds, where the clients' sections ran (free, contended as written,
delegated), from the runtime's report.
"""
import json, random, statistics as st, sys
from collections import defaultdict

DIR = sys.argv[1]
GATE = float(sys.argv[sys.argv.index("--gate") + 1]) if "--gate" in sys.argv else 0.10
rows = [json.loads(l) for l in open(f"{DIR}/runs.jsonl")]
random.seed(1)

def med(x): return st.median(x)
def ci(x, n=2000):
    if len(x) < 3: return (min(x), max(x))
    m = sorted(med(random.choices(x, k=len(x))) for _ in range(n))
    return m[int(0.025 * n)], m[int(0.975 * n) - 1]
def cv(x): return st.pstdev(x) / st.mean(x) if len(x) > 1 and st.mean(x) else 0.0

timed = [r for r in rows if r["tag"] in ("timed", "untimed")]
retired = [r for r in timed if r["busy_out_mean"] > GATE or r["rc"] != 0]
ok = [r for r in timed if r not in retired]
print(f"# d4a {DIR.rstrip(chr(47)).split(chr(47))[-1]}\n")
print(f"runs {len(timed)} measured, {len(retired)} retired (gate {GATE} mean busy outside the set, or rc != 0)")
bo = [r["busy_out_mean"] for r in timed]
print(f"busy outside the set per run: median {med(bo):.4f}, max {max(bo):.4f}; max single processor {max(r['busy_out_max'] for r in timed):.3f}")
fr = sorted(set(r["freq_khz"] for r in rows))
print(f"cpu4 scaling_cur_freq seen: {', '.join(fr[:6])}{' ...' if len(fr) > 6 else ''}")
for r in retired:
    print(f"  retired: rep {r['rep']} T={r['threads']} {r['arm']} busy {r['busy_out_mean']} rc {r['rc']}")

s = [r["ops_s"] for r in rows if r["tag"] == "control-start"]
e = [r["ops_s"] for r in rows if r["tag"] == "control-end"]
if s and e:
    d = med(e) / med(s) - 1
    print(f"\ncontrols (orig, 8 threads, ops/s): start median {med(s):.4g} {[round(x) for x in s]}, end median {med(e):.4g} {[round(x) for x in e]}: {100*d:+.1f}%{'  DRIFT > 5%' if abs(d) > 0.05 else ''}")

g = defaultdict(list)
for r in ok: g[(r["threads"], r["arm"])].append(r)
first, last = [], []
for k, rs in g.items():
    rs = sorted(rs, key=lambda r: r["rep"])
    if len(rs) < 6: continue
    base = rs[0]["ops_s"]; n = len(rs) // 3
    first += [r["ops_s"] / base for r in rs[:n]]; last += [r["ops_s"] / base for r in rs[-n:]]
if first:
    print(f"drift: configurations normalised to their first repetition, median of the first third {med(first):.3f}, of the last third {med(last):.3f}")

arms = []
for r in timed:
    if r["arm"] not in arms: arms.append(r["arm"])
order = ["orig", "orig-aa", "wtc", "deleg", "deleg-always", "adapt", "litl-mcs"]
arms = [a for a in order if a in arms] + [a for a in arms if a not in order]
Ts = sorted(set(t for t, _ in g))
M = {k: med([r["ops_s"] for r in v]) for k, v in g.items()}

print("\n## Throughput, M reads/s total: median [95% CI of the median] (min-max), n; * = CV > 3%\n")
print("| T | " + " | ".join(arms) + " |"); print("|---" * (len(arms) + 1) + "|")
for t in Ts:
    cells = []
    for a in arms:
        v = [r["ops_s"] / 1e6 for r in g.get((t, a), [])]
        if not v: cells.append(""); continue
        lo, hi = ci(v)
        cells.append(f"{med(v):.3f} [{lo:.3f}-{hi:.3f}] ({min(v):.3f}-{max(v):.3f}) n{len(v)}{'*' if cv(v) > 0.03 else ''}")
    print(f"| {t} | " + " | ".join(cells) + " |")

def f(t, a, b): return f"{M[(t, a)] / M[(t, b)]:.2f}" if (t, a) in M and (t, b) in M else ""
print("\n## Ratios of medians\n")
print("| T | aa/orig | wtc/orig | deleg/orig | adapt/orig | adapt/wtc | adapt/deleg | deleg-always/deleg | adapt/litl-mcs | litl-mcs/orig |")
print("|---|---|---|---|---|---|---|---|---|---|")
for t in Ts:
    print(f"| {t} | {f(t,'orig-aa','orig')} | {f(t,'wtc','orig')} | {f(t,'deleg','orig')} | {f(t,'adapt','orig')} | {f(t,'adapt','wtc')} | "
          f"{f(t,'adapt','deleg')} | {f(t,'deleg-always','deleg')} | {f(t,'adapt','litl-mcs')} | {f(t,'litl-mcs','orig')} |")

print("\n## CPU: cores busy = (user+sys)/wall of the process; CPU seconds per million reads; system share; context switches per run (medians)\n")
print("| T | " + " | ".join(arms) + " |"); print("|---" * (len(arms) + 1) + "|")
def cores(r): return (r["usr_s"] + r["sys_s"]) / r["wall_s"]
def cpm(r): return (r["usr_s"] + r["sys_s"]) / (r["reads_per_thread"] * r["threads"] / 1e6)
for t in Ts:
    cells = []
    for a in arms:
        v = g.get((t, a), [])
        if not v: cells.append(""); continue
        sysf = med([r["sys_s"] / (r["usr_s"] + r["sys_s"]) for r in v if r["usr_s"] + r["sys_s"] > 0])
        cells.append(f"{med([cores(r) for r in v]):.1f}c {med([cpm(r) for r in v]):.2f}s/M sys{100*sysf:.0f}% cs{med([r['ctxsw'] for r in v])/1e3:.0f}k")
    print(f"| {t} | " + " | ".join(cells) + " |")

print("\nreads per CPU second (M, medians): adapt / deleg / wtc / orig / litl-mcs")
for t in Ts:
    def pc(a):
        v = g.get((t, a), [])
        return med([1 / cpm(r) for r in v]) if v else None
    vals = [pc(a) for a in ("adapt", "deleg", "wtc", "orig", "litl-mcs")]
    print(f"  T={t}: " + " / ".join(f"{x:.3f}" if x else "-" for x in vals) +
          (f"  (adapt/orig {vals[0]/vals[3]:.2f}, adapt/deleg {vals[0]/vals[1]:.2f})" if vals[0] and vals[1] and vals[3] else ""))

print("\n## Where the clients' sections ran (sums over the repetitions): free / contended as written / delegated\n")
for a in arms:
    for t in Ts:
        v = [r["where"] for r in g.get((t, a), []) if r.get("where")]
        if not v: continue
        w = [sum(x[i] for x in v) for i in range(3)]; tot = sum(w) or 1
        srv = [r["served"] for r in g[(t, a)] if r.get("served")]
        print(f"  {a} T={t}: {w[0]/tot:.3f} / {w[1]/tot:.3f} / {w[2]/tot:.3f}; runs with a server {len(srv)} of {len(g[(t, a)])}")
srv = [r["served"] for r in ok if r["arm"] in ("deleg", "deleg-always") and r["served"]]
print(f"\nserved sections per deleg and deleg-always run: min {min(srv) if srv else 0} max {max(srv) if srv else 0}")

if "--csv" in sys.argv:
    import csv
    with open(sys.argv[sys.argv.index("--csv") + 1], "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["leg", "threads", "arm", "metric", "n", "median", "lo", "hi", "min", "max", "cov"])
        mets = {"mops": lambda r: r["ops_s"] / 1e6, "cores": cores, "cpu_s_per_mread": cpm, "ctxsw": lambda r: r["ctxsw"]}
        for (t, a), v in sorted(g.items()):
            for mn, fn in mets.items():
                x = [fn(r) for r in v]; lo, hi = ci(x)
                w.writerow(["d4a", t, a, mn, len(x), f"{med(x):.6g}", f"{lo:.6g}", f"{hi:.6g}", f"{min(x):.6g}", f"{max(x):.6g}", f"{cv(x):.4f}"])
