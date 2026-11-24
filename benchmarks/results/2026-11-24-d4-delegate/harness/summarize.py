#!/usr/bin/env python3
# Copy of the script the leg ran, with its paths written as $LEGS (the legs' working
# directory), $PROBES (the probes' material), $WTC_D4 / $WTC_D5 (worktrees at the
# measured commits), $WTC (this tree), $LEG_DIR (the host's leg directory).
"""Summaries of the D4 and D5 legs' runs.jsonl.

    summarize.py d4|d5 <leg-dir> [--gate 0.10]

Medians over the repetitions with a bootstrap 95% interval of the median, min-max,
the coefficient of variation (rows above 3% marked *), CPU time, ratios of medians;
the A/A check (orig-aa against orig); the controls (median of three before and after,
5% threshold); a drift check (each configuration normalised to its first repetition:
the median over configurations of the last third against the first third); and the
disturbance gate: runs whose processors outside the leg's set were busy above the
gate on average are retired and listed.
"""
import json, random, statistics as st, sys
from collections import defaultdict

KIND, DIR = sys.argv[1], sys.argv[2]
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
print(f"# {KIND} {DIR.rstrip(chr(47)).split(chr(47))[-1]}\n")
print(f"runs {len(timed)} measured, {len(retired)} retired (gate {GATE} mean busy outside the set, or rc != 0)")
bo = [r["busy_out_mean"] for r in timed]
print(f"busy outside the set per run: median {med(bo):.4f}, max {max(bo):.4f}; max single processor {max(r['busy_out_max'] for r in timed):.3f}")
fr = sorted(set(r["freq_khz"] for r in rows))
print(f"cpu4 scaling_cur_freq seen: {', '.join(fr[:6])}{' ...' if len(fr) > 6 else ''}")
for r in retired:
    print(f"  retired: rep {r['rep']} {r.get('threads', r.get('W'))} {r.get('arm', r.get('var'))} busy {r['busy_out_mean']} rc {r['rc']}")

def controls(metric):
    s = [r[metric] for r in rows if r["tag"] == "control-start"]
    e = [r[metric] for r in rows if r["tag"] == "control-end"]
    if s and e:
        d = med(e) / med(s) - 1
        print(f"\ncontrols ({metric}): start median {med(s):.4g} {s}, end median {med(e):.4g} {e}: {100*d:+.1f}%{'  DRIFT > 5%' if abs(d) > 0.05 else ''}")

def drift(groups):
    first, last = [], []
    for k, rs in groups.items():
        rs = sorted(rs, key=lambda r: r[0])
        if len(rs) < 6: continue
        base = rs[0][1]
        n = len(rs) // 3
        first += [v / base for _, v in rs[:n]]; last += [v / base for _, v in rs[-n:]]
    if first:
        print(f"drift: configurations normalised to their first repetition, median of the first third {med(first):.3f}, of the last third {med(last):.3f}")

if KIND == "d4":
    controls("ops_s")
    g = defaultdict(list)
    for r in ok: g[(r["threads"], r["arm"])].append(r)
    drift({k: [(r["rep"], r["ops_s"]) for r in v] for k, v in g.items()})
    arms = []
    for r in timed:
        if r["arm"] not in arms: arms.append(r["arm"])
    order = ["orig", "orig-aa", "wtc", "deleg", "deleg-float", "hand", "litl-mcs", "litl-clh", "litl-mcsstp"]
    arms = [a for a in order if a in arms] + [a for a in arms if a not in order]
    Ts = sorted(set(t for t, _ in g))
    M = {k: med([r["ops_s"] for r in v]) for k, v in g.items()}
    print("\n## Throughput, M ops/s total: median [95% CI of the median] (min-max), n; * = CV > 3%\n")
    print("| T | " + " | ".join(arms) + " |"); print("|---" * (len(arms) + 1) + "|")
    for t in Ts:
        cells = []
        for a in arms:
            v = [r["ops_s"] / 1e6 for r in g.get((t, a), [])]
            if not v: cells.append(""); continue
            lo, hi = ci(v)
            cells.append(f"{med(v):.3f} [{lo:.3f}-{hi:.3f}] ({min(v):.3f}-{max(v):.3f}) n{len(v)}{'*' if cv(v) > 0.03 else ''}")
        print(f"| {t} | " + " | ".join(cells) + " |")
    print("\n## Ratios of medians\n")
    print("| T | aa/orig | wtc/orig | deleg/orig | deleg/wtc | deleg/hand | deleg/best LiTL (which) | deleg-float/deleg | hand/orig | best LiTL/orig |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for t in Ts:
        o = M.get((t, "orig"))
        lit = {a: M[(t, a)] for a in ("litl-mcs", "litl-clh", "litl-mcsstp") if (t, a) in M}
        bl = max(lit, key=lit.get) if lit else None
        f = lambda a, b: (f"{M[(t, a)] / M[(t, b)]:.2f}" if (t, a) in M and (t, b) in M else "")
        dl = f"{M[(t, 'deleg')] / lit[bl]:.2f} ({bl[5:]})" if bl and (t, "deleg") in M else ""
        lo = f"{lit[bl] / o:.2f}" if bl and o else ""
        print(f"| {t} | {f('orig-aa','orig')} | {f('wtc','orig')} | {f('deleg','orig')} | {f('deleg','wtc')} | {f('deleg','hand')} | "
              f"{dl} | {f('deleg-float','deleg')} | {f('hand','orig')} | {lo} |")
    print("\n## CPU: cores busy = (user+sys)/wall of the process; CPU seconds per million reads; context switches per run (medians)\n")
    print("| T | " + " | ".join(arms) + " |"); print("|---" * (len(arms) + 1) + "|")
    for t in Ts:
        cells = []
        for a in arms:
            v = g.get((t, a), [])
            if not v: cells.append(""); continue
            cores = med([(r["usr_s"] + r["sys_s"]) / r["wall_s"] for r in v])
            cpm = med([(r["usr_s"] + r["sys_s"]) / (r["reads_per_thread"] * r["threads"] / 1e6) for r in v])
            sysf = med([r["sys_s"] / (r["usr_s"] + r["sys_s"]) for r in v if r["usr_s"] + r["sys_s"] > 0])
            cs = med([r["ctxsw"] for r in v])
            cells.append(f"{cores:.1f}c {cpm:.2f}s/M sys{100*sysf:.0f}% cs{cs/1e3:.0f}k")
        print(f"| {t} | " + " | ".join(cells) + " |")
    print("\nper core of CPU time (M reads per CPU second, medians): deleg / hand / orig / best LiTL")
    for t in Ts:
        def pc(a):
            v = g.get((t, a), [])
            return med([(r["reads_per_thread"] * r["threads"] / 1e6) / (r["usr_s"] + r["sys_s"]) for r in v]) if v else None
        lit = {a: M[(t, a)] for a in ("litl-mcs", "litl-clh", "litl-mcsstp") if (t, a) in M}
        bl = max(lit, key=lit.get) if lit else None
        vals = [pc("deleg"), pc("hand"), pc("orig"), pc(bl) if bl else None]
        print(f"  T={t}: " + " / ".join(f"{x:.3f}" if x else "-" for x in vals) + (f"  (deleg/orig {vals[0]/vals[2]:.2f}, deleg/{bl[5:]} {vals[0]/vals[3]:.2f})" if all(vals) else ""))
    srv = [r["served"] for r in ok if r["arm"] == "deleg" and r["served"]]
    print(f"\nserved sections per deleg run: min {min(srv) if srv else 0} max {max(srv) if srv else 0}")
else:
    controls("mtasks_s")
    g = defaultdict(list)
    for r in ok: g[(r["prog"], r["W"], r["inflight"], r["cpus"], r["var"])].append(r)
    drift({k: [(r["rep"], r["mtasks_s"]) for r in v] for k, v in g.items()})
    cells = []
    for r in timed:
        c = (r["prog"], r["W"], r["inflight"], r["cpus"])
        if c not in cells: cells.append(c)
    for over in (0, 1):
        sel = [c for c in cells if any(r["over"] == over for r in timed if (r["prog"], r["W"], r["inflight"], r["cpus"]) == c)]
        vs = []
        for r in timed:
            if r["over"] == over and r["var"] not in vs: vs.append(r["var"])
        print(f"\n## {'Oversubscribed' if over else 'Main (CPUs 4-23)'}: M tasks/s median [95% CI] (min-max), n; * = CV > 3%\n")
        print("| cell | " + " | ".join(vs) + " |"); print("|---" * (len(vs) + 1) + "|")
        for c in sel:
            out = []
            for v in vs:
                x = [r["mtasks_s"] for r in g.get(c + (v,), [])]
                if not x: out.append(""); continue
                lo, hi = ci(x)
                out.append(f"{med(x):.3f} [{lo:.3f}-{hi:.3f}] ({min(x):.3f}-{max(x):.3f}) n{len(x)}{'*' if cv(x) > 0.03 else ''}")
            print(f"| {c[0]} S{4 if c[0]=='multi' else 1} W{c[1]} if{c[2]} {c[3]} | " + " | ".join(out) + " |")
        print(f"\n### ratios of medians to orig; cores busy (cpu_ns/task / ns/task); round trip ns/task for inflight 1\n")
        print("| cell | " + " | ".join(vs) + " |"); print("|---" * (len(vs) + 1) + "|")
        for c in sel:
            o = g.get(c + ("orig",))
            om = med([r["mtasks_s"] for r in o]) if o else None
            out = []
            for v in vs:
                x = g.get(c + (v,), [])
                if not x: out.append(""); continue
                m = med([r["mtasks_s"] for r in x])
                cores = med([r["cpu_ns_task"] / r["ns_task"] for r in x])
                lat = med([r["ns_task"] for r in x])
                out.append(f"x{m/om:.2f} {cores:.1f}c" + (f" {lat:.0f}ns" if c[2] == 1 else "") if om else "")
            print(f"| {c[0]} S{4 if c[0]=='multi' else 1} W{c[1]} if{c[2]} {c[3]} | " + " | ".join(out) + " |")
    print("\n### wtc against the hand patch (ratio of medians)\n")
    for c in cells:
        def m(v):
            x = g.get(c + (v,), []); return med([r["mtasks_s"] for r in x]) if x else None
        w, hs, hp, aa, o = m("wtc"), m("hand-spin"), m("hand-spin_then_park"), m("orig-aa"), m("orig")
        print(f"  {c[0]} W{c[1]} if{c[2]} {c[3]}: wtc/hand-stp4 {w/hp:.2f}  wtc/hand-spin {w/hs:.2f}" + (f"  aa/orig {aa/o:.3f}" if aa else ""))
    bad = [r for r in timed if r["bad"]]
    print(f"\nruns with bad results: {len(bad)}")

if "--csv" in sys.argv:
    import csv
    path = sys.argv[sys.argv.index("--csv") + 1]
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        if KIND == "d4":
            w.writerow(["leg", "threads", "arm", "metric", "n", "median", "lo", "hi", "min", "max", "cov"])
            mets = {"mops": lambda r: r["ops_s"] / 1e6,
                    "cores": lambda r: (r["usr_s"] + r["sys_s"]) / r["wall_s"],
                    "cpu_s_per_mread": lambda r: (r["usr_s"] + r["sys_s"]) / (r["reads_per_thread"] * r["threads"] / 1e6),
                    "ctxsw": lambda r: r["ctxsw"]}
            for (t, a), v in sorted(g.items()):
                for mn, fn in mets.items():
                    x = [fn(r) for r in v]; lo, hi = ci(x)
                    w.writerow(["d4", t, a, mn, len(x), f"{med(x):.6g}", f"{lo:.6g}", f"{hi:.6g}", f"{min(x):.6g}", f"{max(x):.6g}", f"{cv(x):.4f}"])
        else:
            w.writerow(["leg", "prog", "submitters", "workers", "inflight", "cpus", "variant", "metric", "n", "median", "lo", "hi", "min", "max", "cov"])
            mets = {"mtasks_s": lambda r: r["mtasks_s"], "ns_task": lambda r: r["ns_task"],
                    "cores": lambda r: r["cpu_ns_task"] / r["ns_task"], "cpu_ns_task": lambda r: r["cpu_ns_task"],
                    "vcsw_task": lambda r: r["vcsw_task"]}
            for (p, W, inf, cpus, var), v in sorted(g.items()):
                for mn, fn in mets.items():
                    x = [fn(r) for r in v]; lo, hi = ci(x)
                    w.writerow(["d5", p, 4 if p == "multi" else 1, W, inf, cpus, var, mn, len(x), f"{med(x):.6g}", f"{lo:.6g}", f"{hi:.6g}", f"{min(x):.6g}", f"{max(x):.6g}", f"{cv(x):.4f}"])
