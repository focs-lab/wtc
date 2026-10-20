#!/usr/bin/env python3
"""Turn a results directory's runs.jsonl into SUMMARY.md.

Groups runs by configuration, reports the median across repetitions with
a bootstrap 95% confidence interval of the median, flags rows whose
coefficient of variation exceeds 3%, computes ratios against the
general-purpose shim (B0) and against the inline ring (the ceiling), and
checks that the opening and closing control runs agree within 5%.

Runs of an explaining pass (a "pass" field, e.g. mechanism) never enter the
timing tables or the drift checks; they get tables of their own, per
element pushed: allocations, hardware events, peak memory.

    summarise.py <results-dir>        # Markdown to stdout, summary.csv beside runs.jsonl
"""
import csv
import json
import random
import statistics
import sys
from collections import defaultdict
from pathlib import Path

# The denominator of every ratio is whichever general-purpose build this
# workload has: the adapter legs call it shim_generic, the compiler-selected
# leg calls its untouched build generic. Naming one and leaving the columns
# empty when it is absent is how a table comes out with no ratios at all.
BASELINE_CANDIDATES = ("shim_generic", "generic", "orig_generic", "plain")
CEILING_CANDIDATES = ("ring_inline", "rigtorp_inline")


def pick(variants, candidates):
    for name in candidates:
        if name in variants:
            return name
    return None


def load(path):
    runs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                runs.append(json.loads(line))
    return runs


# What makes two runs the same configuration. A field a workload does not
# write is empty, so one key serves every workload; leaving one out is how
# runs at different rates or batch sizes get averaged together.
CONFIG_FIELDS = ("workload", "mode", "elem", "wait", "interval_ns", "rate", "smt", "placement",
                 "threads", "producers", "consumers", "work", "pass")
# Recorded with every median in summary.csv, but not part of the key: the
# batch size is in a batching flavour's name, and keying on it would put a
# plain loop and its batched build in different tables.
CSV_EXTRA = ("k",)


def key_of(r):
    return tuple(str(r.get(f, "")) if f != "smt" else str(r.get("smt", 0)) for f in CONFIG_FIELDS)


def title_of(key):
    k = dict(zip(CONFIG_FIELDS, key))
    title = f"## {k['workload']}{(' ' + k['mode']) if k['mode'] else ''}"
    if k["pass"]:
        title += f"  [{k['pass']} pass]"
    if k["elem"]:
        title += f"  E{k['elem']}"
    for f in ("threads", "producers", "consumers", "work", "wait"):
        if k[f]:
            title += f"  {f}={k[f]}"
    if k["interval_ns"] and k["interval_ns"] != "0":
        title += f"  interval={k['interval_ns']} ns"
    if k["rate"] and k["rate"] != "0":
        title += f"  rate={int(float(k['rate'])):,}/s"
    if k["placement"]:
        title += f"  ({k['placement']})"
    elif k["smt"] == "1":
        title += "  (SMT siblings)"
    return title


# Percentile bootstrap of the median, in plain Python: the summary has to
# run wherever a leg ran, and the container the evaluation builds in has no
# numpy. Seeded, so a summary regenerated from the same runs is the same.
def boot_ci(values, iters=2000, seed=1):
    rng = random.Random(seed)
    a = [float(v) for v in values]
    if len(a) < 2:
        return (min(a), max(a))
    meds = sorted(statistics.median(rng.choices(a, k=len(a))) for _ in range(iters))
    return (meds[int(0.025 * (iters - 1))], meds[int(0.975 * (iters - 1))])


def summarise_metric(rows, metric):
    vals = [float(r[metric]) for r in rows if metric in r]
    if not vals:
        return None
    med = statistics.median(vals)
    cov = (statistics.pstdev(vals) / statistics.mean(vals)) if len(vals) > 1 and statistics.mean(vals) else 0.0
    lo, hi = boot_ci(vals)
    return dict(n=len(vals), median=med, min=min(vals), max=max(vals), cov=cov, lo=lo, hi=hi)


def fmt_ratio(x):
    """A ratio, readable at both ends. A generic queue that cannot keep up
    shows a tail a million times the specialised one's, and "0.00x" is a
    true statement that tells the reader nothing."""
    if x is None:
        return "-"
    if x < 0.01:
        return f"1/{1/x:,.0f}"
    if x >= 100:
        return f"{x:,.0f}×"
    return f"{x:.2f}×"


def fmt(x, unit=""):
    if x is None:
        return "-"
    if abs(x) >= 100:
        return f"{x:,.0f}{unit}"
    return f"{x:.2f}{unit}"


def main(out_dir):
    out = Path(out_dir)
    runs = load(out / "runs.jsonl")
    meta = json.loads((out / "meta.json").read_text()) if (out / "meta.json").exists() else {}
    controls = [r for r in runs if r.get("rep") in (900, 901) and not r.get("pass")]
    explaining = [r for r in runs if r.get("pass")]
    runs = [r for r in runs if r.get("rep") not in (900, 901) and not r.get("pass")]

    print(f"# {meta.get('leg', out.name)}  ({meta.get('date', '')})\n")
    if meta:
        print(f"{meta.get('cpu_model','')}, kernel {meta.get('kernel','')}, allowed cpus {meta.get('allowed_cpus','')}, "
              f"smt {meta.get('smt_control','')}, build `{meta.get('build','')}`, git {meta.get('git_sha','')[:12]}"
              f"{' (dirty)' if meta.get('git_dirty') else ''}\n")

    # Two drift checks, because the first one on its own lies. Comparing a
    # single opening run with a single closing run of a variant whose own
    # run-to-run spread is ~8% flags every leg and says nothing; medians of
    # three, against a threshold that respects that spread, say something.
    opening = [float(r["mops"]) for r in controls if r.get("rep") == 900]
    closing = [float(r["mops"]) for r in controls if r.get("rep") == 901]
    control_drift = None
    if opening and closing:
        a, b = statistics.median(opening), statistics.median(closing)
        control_drift = (a, b, abs(a - b) / max(a, b))

    # The sensitive check: every configuration measured in every repetition,
    # each normalised to its own first repetition. Tens of ratios instead of
    # two numbers, so a real trend shows and ordinary noise does not.
    # Throughput where a leg has it, median latency where it does not: a
    # trend shows in either, and a latency leg would otherwise go unchecked.
    by_cfg = defaultdict(dict)
    for r in runs:
        if r.get("rep", 0) > 100:
            continue
        metric = "mops" if "mops" in r else ("p50" if "p50" in r else None)
        if metric is None:
            continue
        by_cfg[key_of(r) + (r["variant"],)][r["rep"]] = float(r[metric])
    reps = sorted({rep for d in by_cfg.values() for rep in d})
    if len(reps) > 1:
        cells = []
        for rep in reps:
            ratios = [d[rep] / d[reps[0]] for d in by_cfg.values() if reps[0] in d and rep in d and d[reps[0]]]
            if ratios:
                cells.append((rep, statistics.median(ratios), len(ratios)))
        # Drift means a consistent direction, not merely a spread: a leg whose
        # repetitions wobble either side of the first is noisy, and one whose
        # repetitions climb or fall throughout has something changing under
        # it, which is the case that invalidates ratios measured at different
        # times. Only the second is worth a flag.
        vals = [m for _, m, _ in cells]
        rising = all(b >= a for a, b in zip(vals, vals[1:]))
        falling = all(b <= a for a, b in zip(vals, vals[1:]))
        span = max(vals) - min(vals)
        if (rising or falling) and span > 0.03:
            flag = "**drifting in one direction: treat ratios measured far apart with care**"
        elif span > 0.05:
            flag = f"no trend, but a wide spread ({span*100:.1f}%)"
        else:
            flag = "no trend"
        # "Settled" needs both: no direction and no great width. A leg whose
        # repetitions wander over 20% is not settled just because they wander
        # both ways, and calling it so would excuse exactly the legs worth
        # looking at twice.
        clean = not ((rising or falling) and span > 0.03) and span <= 0.05
        if control_drift:
            a, b, d = control_drift
            if d <= 0.05:
                verdict = "OK"
            elif clean and b > a:
                # The two disagree and the repetitions do not: the opening
                # controls ran before the machine had settled. Say which
                # check to believe rather than flagging a usable leg.
                verdict = ("opening controls low, repetitions flat: a warm-up that ended early, "
                           "not drift; the leg is usable")
            else:
                verdict = "**NOISY: rerun this leg**"
            print(f"Control (shim_generic E64, median of {len(opening)} and {len(closing)}): "
                  f"open {a:.2f}, close {b:.2f} Mops/s, drift {d*100:.1f}% — {verdict}\n")
        # Name the widest row. A leg is one thing and a variant is another:
        # the inline ring has a slow regime of its own and the mutex queues
        # have erratic tails, and either will widen a leg's spread without
        # anything having changed about the machine. A reader who can see
        # which row is wide can tell those apart; a bare leg-level flag
        # cannot.
        widest = max(((max(d[r] / d[reps[0]] for r in d) - min(d[r] / d[reps[0]] for r in d), k)
                      for k, d in by_cfg.items() if reps[0] in d and len(d) > 1 and d[reps[0]]),
                     default=(0, None))
        note = ""
        if widest[1] is not None and widest[0] > 0.10:
            wk = widest[1]
            note = (f"  Widest single row: {wk[-1]} in {title_of(wk[:-1])[3:].strip()}"
                    + f", {widest[0]*100:.0f}% across repetitions.")
        print("Drift across repetitions, median over all configurations, relative to the first: "
              + ", ".join(f"rep {rep} {m:.3f}" for rep, m, _ in cells)
              + f" (n={cells[0][2]}) — {flag}." + note + "\n")

    groups = defaultdict(lambda: defaultdict(list))
    for r in runs:
        groups[key_of(r)][r["variant"]].append(r)
    csv_rows = []

    for key in sorted(groups):
        print(title_of(key) + "\n")
        variants = groups[key]
        base_name = pick(variants, BASELINE_CANDIDATES)
        ceil_name = pick(variants, CEILING_CANDIDATES)
        base = variants.get(base_name) if base_name else None
        ceil = variants.get(ceil_name) if ceil_name and ceil_name != base_name else None
        # Which columns a workload has is a property of the workload, not
        # something to assume: the throughput workload reports no
        # percentiles, the pipeline reports both throughput and tails but
        # not every percentile, and assuming otherwise is what made this
        # script crash mid-campaign and take the two legs after it down.
        present = [m for m in ("mops", "p50", "p90", "p99", "p999", "p9999", "max_ns", "finish_spread")
                   if any(summarise_metric(rows, m) for rows in variants.values())]
        if not present:
            print("_no usable measurements_\n")
            continue
        sort_key, ascending = (("mops", False) if "mops" in present
                               else ("p99" if "p99" in present else present[0], True))

        titles = {"mops": "Mops/s", "max_ns": "max", "p999": "p99.9", "p9999": "p99.99",
                  "finish_spread": "finish spread"}
        for v, rows in variants.items():
            for m in present:
                st = summarise_metric(rows, m)
                if st:
                    extra = {f: str(rows[0].get(f, "")) for f in CSV_EXTRA}
                    sat = sum(int(r.get("saturated", 0)) for r in rows)
                    csv_rows.append(dict(zip(CONFIG_FIELDS, key), variant=v, metric=m, saturated=sat, **extra, **st))
        head = ["variant", "n"] + [titles.get(m, m) for m in present]
        if "mops" in present:
            head.insert(head.index("Mops/s") + 1, "ns/op")
        ratio_cols = [m for m in ("mops", "p99", "p999") if m in present]
        head += [f"{titles.get(m, m)} vs {base_name}" for m in ratio_cols] if base else []
        if "mops" in present and ceil:
            head.append(f"vs {ceil_name}")
        print("| " + " | ".join(head) + " |")
        print("|" + "---|" * len(head))

        def med(rows, m):
            s = summarise_metric(rows, m)
            return s["median"] if s else None

        order = sorted(variants, key=lambda v: (med(variants[v], sort_key) or 0) * (1 if ascending else -1))
        for v in order:
            cells = []
            n = 0
            for m in present:
                s = summarise_metric(variants[v], m)
                if not s:
                    cells.append("-")
                    continue
                n = max(n, s["n"])
                flag = " ⚠" if m == sort_key and s["cov"] > 0.03 else ""
                spread = ""
                if m == sort_key:
                    spread = f" [{fmt(s['lo'])}–{fmt(s['hi'])}]"
                if m == "finish_spread":   # a share of the run
                    cells.append(f"{100 * s['median']:.1f}% (max {100 * s['max']:.1f}%)")
                    continue
                sat = " sat" if m == "p50" and any(int(r.get("saturated", 0)) for r in variants[v]) else ""
                cells.append(f"{fmt(s['median'])}{flag}{spread}{sat}")
                if m == "mops":
                    cells.append(fmt(1000.0 / s["median"]) if s["median"] else "-")
            if base:
                for m in ratio_cols:
                    a, b = med(variants[v], m), med(base, m)
                    cells.append(fmt_ratio(a / b) if a and b else "-")
            if "mops" in present and ceil:
                a, c = med(variants[v], "mops"), med(ceil, "mops")
                cells.append(fmt_ratio(a / c) if a and c else "-")
            print("| " + " | ".join([v, str(n)] + cells) + " |")
        print()
    explain(explaining, csv_rows)
    write_csv(out / "summary.csv", csv_rows)
    print("Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of "
          "the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. "
          "Latencies in ns; tails are the median across runs of each run's own percentile. "
          "\"sat\" marks a row with a run that did not sustain its offered rate: its latency is a backlog.")


# Per element pushed, counting warm-up, since the counters cover the run.
PER_ELEMENT = (("allocs", "allocs/elem"), ("frees", "frees/elem"), ("cycles", "cycles/elem"),
               ("instructions", "instr/elem"), ("cache_misses", "cache misses/elem"),
               ("llc_read_misses", "LLC read misses/elem"))


def explain(runs, csv_rows):
    groups = defaultdict(lambda: defaultdict(list))
    for r in runs:
        groups[key_of(r)][r["variant"]].append(r)
    for key in sorted(groups):
        variants = groups[key]
        per = {}
        for v, rows in variants.items():
            for m, _ in PER_ELEMENT:
                vals = [float(r[m]) / float(r["n_total"]) for r in rows if m in r and r.get("n_total")]
                if vals:
                    per[(v, m)] = summarise_metric([{m: x} for x in vals], m)
            rss = summarise_metric(rows, "peak_rss_kib")
            if rss:
                per[(v, "peak_rss_kib")] = rss
        cols = [(m, t) for m, t in PER_ELEMENT if any((v, m) in per for v in variants)]
        if not cols:
            continue
        print(title_of(key) + "\n")
        head = ["variant", "n"] + [t for _, t in cols] + ["IPC", "peak RSS MiB"]
        print("| " + " | ".join(head) + " |")
        print("|" + "---|" * len(head))
        for v in sorted(variants):
            n = len(variants[v])
            cells = [fmt(per[(v, m)]["median"]) if (v, m) in per else "-" for m, _ in cols]
            cyc, ins = per.get((v, "cycles")), per.get((v, "instructions"))
            cells.append(f"{ins['median'] / cyc['median']:.2f}" if cyc and ins and cyc["median"] else "-")
            rss = per.get((v, "peak_rss_kib"))
            cells.append(fmt(rss["median"] / 1024.0) if rss else "-")
            print("| " + " | ".join([v, str(n)] + cells) + " |")
            for (vv, m), st in per.items():
                if vv == v:
                    csv_rows.append(dict(zip(CONFIG_FIELDS, key), variant=v, metric=m + "_per_elem"
                                         if m != "peak_rss_kib" else m, **st))
        print()


def write_csv(path, rows):
    if not rows:
        return
    fields = list(CONFIG_FIELDS) + list(CSV_EXTRA) + ["variant", "metric", "n", "median", "lo", "hi", "min", "max", "cov",
                                                      "saturated"]
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in fields})


if __name__ == "__main__":
    main(sys.argv[1])
