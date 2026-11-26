#!/usr/bin/env python3
"""Summary of the progress runtime's queue leg (leg_pr72.py).

    summarize_pr72.py <leg-dir> [--gate 0.10]   writes SUMMARY.md and summary.csv there

Medians of the throughput (Mops/s; W11: M tasks/s) over the repetitions with a
bootstrap 95% interval of the median, min-max, the coefficient of variation
(cells above 3% marked *); the A/A check (main-aa against main); pr/main as a
ratio of medians with a bootstrap 95% interval of that ratio; a cell is
flagged when pr/main is more than 3% from 1, its interval excludes 1 and it
is further from 1 than the cell's A/A. The controls (median of three before
and after, 5% threshold); a drift check (each configuration normalised to its
first repetition: the median over configurations of the last third against
the first third); the disturbance gate (runs whose processors outside the
leg's set were busy above the gate on average are retired and listed; in an
untimed leg the gate is off unless given). Then the rings of indices (scq,
wcq) against the bounded ring with the same tag-free shape (generic), and
against the ring of the topology that fits the cell where there is one.
"""
import csv, json, random, statistics as st, sys
from collections import defaultdict

DIR = sys.argv[1].rstrip("/")
rows = [json.loads(l) for l in open(f"{DIR}/runs.jsonl")]
MODE = next((r["mode"] for r in rows), "untimed")
GATE = float(sys.argv[sys.argv.index("--gate") + 1]) if "--gate" in sys.argv else (0.10 if MODE == "timed" else 9.9)
random.seed(1)
out = []
p = out.append


def med(x): return st.median(x)
def ci(x, n=2000):
    if len(x) < 3: return (min(x), max(x))
    m = sorted(med(random.choices(x, k=len(x))) for _ in range(n))
    return m[int(0.025 * n)], m[int(0.975 * n) - 1]
def ratio_ci(a, b, n=2000):
    if len(a) < 3 or len(b) < 3: return (float("nan"), float("nan"))
    m = sorted(med(random.choices(a, k=len(a))) / med(random.choices(b, k=len(b))) for _ in range(n))
    return m[int(0.025 * n)], m[int(0.975 * n) - 1]
def cv(x): return st.pstdev(x) / st.mean(x) if len(x) > 1 and st.mean(x) else 0.0
def val(r): return r["result"]["mops"]


meas = [r for r in rows if r["tag"] in ("timed", "untimed")]
bad = [r for r in meas if r["rc"] != 0 or not r["result"]]
retired = [r for r in meas if r in bad or r["busy_out_mean"] > GATE]
ok = [r for r in meas if r not in retired]
p(f"# pr72 {DIR.split('/')[-1]} ({MODE})\n")
p(f"runs {len(meas)} measured, {len(retired)} retired (rc != 0 or no result: {len(bad)}; gate {GATE} mean busy outside the set)")
bo = [r["busy_out_mean"] for r in meas]
p(f"busy outside the set per run: median {med(bo):.4f}, max {max(bo):.4f}; max single processor {max(r['busy_out_max'] for r in meas):.3f}")
fr = sorted(set(r["freq_khz"] for r in rows))
p(f"scaling_cur_freq of the set's first processor seen: {', '.join(fr[:6])}{' ...' if len(fr) > 6 else ''}")
for r in retired[:40]:
    p(f"  retired: rep {r['rep']} {r['key']} {r['arm']} busy {r['busy_out_mean']} rc {r['rc']}")

s = [val(r) for r in rows if r["tag"] == "control-start" and r["result"]]
e = [val(r) for r in rows if r["tag"] == "control-end" and r["result"]]
if s and e:
    d = med(e) / med(s) - 1
    p(f"\ncontrols (W1 shim_generic E64, main, Mops/s): start median {med(s):.3f} {[round(x, 2) for x in s]}, "
      f"end median {med(e):.3f} {[round(x, 2) for x in e]}: {100 * d:+.1f}%{'  DRIFT > 5%' if abs(d) > 0.05 else ''}")

g = defaultdict(list)
for r in ok: g[(r["key"], r["arm"])].append(r)
first, last = [], []
for k, rs in g.items():
    rs = sorted(rs, key=lambda r: r["rep"])
    if len(rs) < 3: continue
    base = val(rs[0]); n = max(1, len(rs) // 3)
    first += [val(r) / base for r in rs[:n]]; last += [val(r) / base for r in rs[-n:]]
if first:
    p(f"drift: configurations normalised to their first repetition, median of the first third {med(first):.3f}, of the last third {med(last):.3f}")

V = {k: [val(r) for r in v] for k, v in g.items()}
keys = []
for r in meas:
    if r["key"] not in keys: keys.append(r["key"])


def fmt(v):
    if not v: return ""
    lo, hi = ci(v)
    return f"{med(v):.3f} [{lo:.3f}-{hi:.3f}] ({min(v):.3f}-{max(v):.3f}) n{len(v)}{'*' if cv(v) > 0.03 else ''}"


csvrows = []
p("\n## main against the progress runtime (pr), Mops/s (W11: M tasks/s): median [95% CI of the median] (min-max), n; * = CV > 3%\n")
p("| cell | main | main-aa | pr | aa/main | pr/main [95% CI] | |")
p("|---|---|---|---|---|---|---|")
flags = []
for k in keys:
    m, a, q = V.get((k, "main"), []), V.get((k, "main-aa"), []), V.get((k, "pr"), [])
    if not m or not q: continue
    rq = med(q) / med(m); ra = med(a) / med(m) if a else float("nan")
    lo, hi = ratio_ci(q, m)
    flag = lo == lo and abs(rq - 1) > 0.03 and not (lo <= 1 <= hi) and (not a or abs(rq - 1) > abs(ra - 1))
    if flag: flags.append((k, rq, lo, hi, ra))
    p(f"| {k} | {fmt(m)} | {fmt(a)} | {fmt(q)} | {ra:.3f} | {rq:.3f} [{lo:.3f}-{hi:.3f}] | {'**moved**' if flag else ''} |")
    csvrows.append(dict(cell=k, main=med(m), main_aa=med(a) if a else "", pr=med(q), aa_main=ra, pr_main=rq,
                        pr_main_lo=lo, pr_main_hi=hi, cv_main=cv(m), cv_pr=cv(q), n_main=len(m), n_pr=len(q), flag=flag))
p("")
if flags:
    p("Cells that moved (|pr/main - 1| > 3%, interval excluding 1, beyond the cell's A/A):\n")
    for k, rq, lo, hi, ra in flags:
        p(f"- {k}: pr/main {rq:.3f} [{lo:.3f}-{hi:.3f}], aa/main {ra:.3f}")
else:
    p("No cell moved by more than 3% beyond its interval and its A/A.")

p("\n## The rings of indices against the bounded ring (pr's build), Mops/s: median (min-max), n\n")
p("| shape | ring, generic | ring, fitting tag | scq | wcq | scq/ring | wcq/ring | scq/fitting | wcq/fitting |")
p("|---|---|---|---|---|---|---|---|---|")


def fitting(shape):
    mode, pc = shape.split(" ", 1)
    if mode != "split": return None
    pr_, co = pc.split(":")
    if pr_ == "1" and co == "1": return f"split bounded_spsc {pc}"
    if co == "1": return f"split bounded_mpsc {pc}"
    if pr_ == "1": return f"split bounded_spmc {pc}"
    return None


def short(v): return f"{med(v):.3f} ({min(v):.3f}-{max(v):.3f}) n{len(v)}{'*' if cv(v) > 0.03 else ''}" if v else ""


for k in keys:
    if not k.startswith("WB ") or "bounded_generic" not in k: continue
    shape = k[3:].replace("bounded_generic ", "")
    ring = V.get((k, "pr"), [])
    sk, wk = k.replace("bounded_generic", "bounded_scq"), k.replace("bounded_generic", "bounded_wcq")
    sc, wc = V.get((sk, "pr"), []), V.get((wk, "pr"), [])
    fk = fitting(shape)
    fv = V.get(("WB " + fk, "pr"), []) if fk else []
    if not ring: continue
    r_ = lambda a, b: f"{med(a) / med(b):.2f}" if a and b else ""
    fname = fk.split()[1].replace("bounded_", "") if fk else ""
    p(f"| {shape} | {short(ring)} | {fname + ' ' + short(fv) if fv else ''} | {short(sc)} | {short(wc)} | {r_(sc, ring)} | {r_(wc, ring)} | {r_(sc, fv)} | {r_(wc, fv)} |")
    csvrows.append(dict(cell=f"WB {shape} rings", ring=med(ring), fitting=med(fv) if fv else "", scq=med(sc) if sc else "",
                        wcq=med(wc) if wc else ""))

open(f"{DIR}/SUMMARY.md", "w").write("\n".join(out) + "\n")
with open(f"{DIR}/summary.csv", "w", newline="") as f:
    cols = []
    for r in csvrows:
        for c in r:
            if c not in cols: cols.append(c)
    w = csv.DictWriter(f, fieldnames=cols); w.writeheader(); w.writerows(csvrows)
print("\n".join(out))
