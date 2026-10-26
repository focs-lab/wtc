"""Summarise a run.sh directory: per job the median metric, spread and
effective frequency; per pair the ratio within each round, with a bootstrap
95% interval of its median. Higher is better for Mops/s; for latency and
elapsed time the ratio is old/new, so above 1 means the fix helped."""
import collections, json, random, re, statistics as st, sys

out = sys.argv[1]
rows = collections.defaultdict(dict)   # job -> round -> (metric, ghz, outside_busy)
import os
if os.path.exists(f"{out}/runs.tsv"):    # the committed table of every run
    for line in list(open(f"{out}/runs.tsv"))[1:]:
        r, job, metric, val, ghz, busy = line.rstrip("\n").split("\t")
        if metric != "exit":
            rows[job][int(r)] = (float(val), float(ghz), float(busy))
for line in (open(f"{out}/index.txt") if not os.path.exists(f"{out}/runs.tsv") else []):
    f = line.split()
    if len(f) != 5:
        continue
    r, job, rc, b, t = int(f[0]), f[1], int(f[2]), int(f[3]), int(f[4])
    base = f"{out}/runs/r{r}_{job}"
    if rc != 0:
        print(f"round {r} {job}: exit {rc}", file=sys.stderr); continue
    perf = {}
    for pl in open(base + ".perf"):
        p = pl.split(",")
        if len(p) > 2 and p[0].replace(".", "").isdigit():
            perf[p[2].split(":")[0]] = float(p[0])
    ghz = perf.get("cycles", 0) / (perf.get("task-clock", 1) * 1e6)
    if job.startswith("lr_"):
        m = re.search(r"elapsed_ms ([0-9.]+)", open(base + ".err").read())
        val = float(m.group(1))
    else:
        j = json.loads(open(base + ".json").readline())
        val = j["p50"] if job.startswith("w2_") else j["mops"]
    rows[job][r] = (val, ghz, b / max(t, 1))

def boot(xs, k=4000):
    m = sorted(st.median(random.choices(xs, k=len(xs))) for _ in range(k))
    return m[int(.025 * k)], m[int(.975 * k)]

print(f"{'job':34} {'n':>3} {'median':>10} {'CV%':>5} {'GHz':>5} {'outside':>7}")
for job in sorted(rows):
    v = [x[0] for x in rows[job].values()]
    print(f"{job:34} {len(v):3} {st.median(v):10.3f} {100*st.pstdev(v)/st.mean(v):5.1f} "
          f"{st.median(x[1] for x in rows[job].values()):5.2f} {st.median(x[2] for x in rows[job].values()):7.3f}")

def pair(new, old, lower_better):
    A, B = rows.get(new, {}), rows.get(old, {})
    rs = [(B[r][0] / A[r][0]) if lower_better else (A[r][0] / B[r][0]) for r in A if r in B]
    if len(rs) < 3:
        return
    lo, hi = boot(rs)
    print(f"{new:30} vs {old:30} {st.median(rs):6.3f}  [{lo:.3f}, {hi:.3f}]  n={len(rs)}")

print("\nratios within a round (>1 = the first is better)")
for impl in ("inline", "generic"):
    for e in ("e8", "e64"):
        pair(f"w7_fix_{impl}_{e}", f"w7_pre_{impl}_{e}", False)
        pair(f"w7_prealign_{impl}_{e}", f"w7_pre_{impl}_{e}", False)
        pair(f"w7_prealign_{impl}_{e}", f"w7_fix_{impl}_{e}", False)
for v in ("rigtorp_inline", "shim_spsc_inline", "shim_generic"):
    pair(f"w3_main_{v}", f"w3_pre_{v}", False)
for v in ("shim_spsc_inline", "rigtorp_inline"):
    pair(f"w2_main_{v}", f"w2_pre_{v}", True)   # p50 latency
for o in ("O2", "O0"):
    pair(f"lr_pad_{o}", f"lr_orig_{o}", True)   # elapsed time
