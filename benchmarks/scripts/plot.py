#!/usr/bin/env python3
"""Figures from a results directory: what the evaluation shows, drawn the
same way every time.

    plot.py bars  <dir> [--workload W1] [--metric mops] [--by elem]
    plot.py load  <dir> [--workload W2] [--metric p99]
    plot.py sweep <dir> --x threads [--workload W9] [--metric mops] [--log]
    plot.py cdf   <dir> [--workload W2] [--where interval_ns=1000]

bars, load and sweep read summary.csv, which summarise.py writes beside
runs.jsonl: medians with their bootstrap 95% intervals, drawn as error
bars, so no figure can show a difference its interval does not support.
cdf reads each run's latency histogram from runs.jsonl and pools the runs
of a variant. Every figure goes to <dir>/plots/ as SVG (--format pdf for a
paper, png to look at), named after its command, and the same variant
always gets the same colour: the shim's general-purpose queue grey, its
spsc ring blue, inline orange, the libraries in muted tones. The family
is in the line too, so a figure still reads in grey: the shim and the
compiler's builds solid, the general-purpose libraries (B1) dashed, the
single-producer libraries (B2) dash-dotted, the bare rings dotted. A point
any of whose runs saturated is drawn open, and a bar is marked "sat":
past saturation the latency grows with the length of the run, so the
number is a lower bound, not a property of the queue.
"""
import argparse
import csv
import json
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402
from matplotlib.ticker import EngFormatter, FuncFormatter, NullFormatter  # noqa: E402

COLOURS = {
    "shim_generic": "#8a96a6", "generic": "#8a96a6", "orig_generic": "#b7c0cc", "plain": "#8a96a6",
    "shim_spsc": "#2f6fb2", "hand": "#2f6fb2",
    "shim_spsc_inline": "#c2562a", "compiler": "#c2562a", "hand-inline": "#e8a27e", "batched": "#c2562a",
    "shim_mpsc": "#167f8a", "shim_spmc": "#b23a6f",
    "ring_inline": "#1a1a1a", "ring_borrowed": "#5a5a5a", "ring_sp": "#5d8fc4",
    "onetbb_cq_sp": "#1d5c3a", "moodycamel_cq_sp": "#4f9a6a", "boost_ms_queue": "#8cc49f", "std_mutex": "#9a9a4e",
    "rigtorp_sp": "#6b4f91", "rigtorp_inline": "#8e6fb8", "moodycamel_rw_sp": "#b08a3e",
    "moodycamel_rw_inline": "#d1ad5e", "boost_spsc_sp": "#c7a7d6",
}
# Within a family the marker tells the variants apart.
MARKERS = {"orig_generic": "s", "onetbb_cq_sp": "^", "moodycamel_cq_sp": "v", "boost_ms_queue": "D",
           "std_mutex": "X", "rigtorp_inline": "^", "rigtorp_sp": "^", "moodycamel_rw_inline": "v",
           "moodycamel_rw_sp": "v", "boost_spsc_sp": "D", "ring_borrowed": "s", "ring_sp": "D",
           "hand": "s", "hand-inline": "s"}
LABELS = {"mops": "million elements a second", "p50": "p50 latency (ns)", "p99": "p99 latency (ns)",
          "p999": "p99.9 latency (ns)", "ns_per_op": "ns per element"}


B1 = ("onetbb_", "moodycamel_cq", "boost_ms", "std_mutex")
B2 = ("rigtorp_", "moodycamel_rw", "boost_spsc")
FORMAT = "svg"


def colour(v):
    return COLOURS.get(v, "#444444")


def family(v):
    if v.startswith(B1):
        return "--", "//"
    if v.startswith(B2):
        return "-.", "\\\\"
    if v.startswith("ring_"):
        return ":", ".."
    return "-", None


def read_summary(d):
    path = Path(d) / "summary.csv"
    if not path.exists():
        sys.exit(f"{path} missing: run summarise.py first")
    with open(path) as f:
        return list(csv.DictReader(f))


def select(rows, workload, metric, where):
    out = []
    for r in rows:
        if workload and r["workload"] != workload:
            continue
        if r["metric"] != metric or r.get("pass"):
            continue
        if any(r.get(k, "") != v for k, v in where.items()):
            continue
        out.append(r)
    return out


def one_per(rows, key, what):
    # A figure draws one value per variant and position; two rows there mean
    # the selection left a configuration field open, and drawing either would
    # hide the other.
    seen = {}
    for r in rows:
        k = (r["variant"], key(r))
        if k in seen:
            differ = sorted(f for f in r if f not in ("median", "lo", "hi", "min", "max", "n", "cov") and r[f] != seen[k][f])
            at = f"{k[1]:g}" if isinstance(k[1], float) else k[1]
            sys.exit(f"{r['variant']} has two rows at {what}={at}; narrow with --where on {', '.join(differ)}")
        seen[k] = r


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def tag_of(where):
    return "".join(f"-{k}{v}" for k, v in where.items())


def save(fig, d, name):
    out = Path(d) / "plots"
    out.mkdir(exist_ok=True)
    path = out / f"{name}.{FORMAT}"
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    print(path)


def bars(a):
    rows = select(read_summary(a.dir), a.workload, a.metric, a.where)
    one_per(rows, lambda r: r.get(a.by, ""), a.by)
    groups = defaultdict(dict)
    for r in rows:
        groups[r.get(a.by, "")][r["variant"]] = r
    variants = sorted({v for g in groups.values() for v in g},
                      key=lambda v: -max(num(g[v]["median"]) or 0 for g in groups.values() if v in g))
    keys = sorted(groups, key=lambda k: num(k) if num(k) is not None else k)
    fig, ax = plt.subplots(figsize=(min(11, max(6, 0.4 * len(variants) * len(keys))), 4))
    width = 0.8 / max(1, len(variants))
    for i, v in enumerate(variants):
        xs, ys, lo, hi = [], [], [], []
        for j, k in enumerate(keys):
            r = groups[k].get(v)
            if not r:
                continue
            m = num(r["median"])
            xs.append(j + i * width)
            ys.append(m)
            lo.append(m - num(r["lo"]))
            hi.append(num(r["hi"]) - m)
            if num(r.get("saturated")):
                ax.annotate("sat", (xs[-1], num(r["hi"])), xytext=(0, 2), textcoords="offset points",
                            ha="center", va="bottom", fontsize=6, rotation=90)
        ax.bar(xs, ys, width, yerr=[lo, hi], capsize=2, color=colour(v), hatch=family(v)[1],
               edgecolor="white", linewidth=0, label=v)
    ax.set_xticks([j + 0.4 - width / 2 for j in range(len(keys))])
    ax.set_xticklabels([f"{a.by}={k}" if k else "" for k in keys])
    ax.set_ylabel(LABELS.get(a.metric, a.metric))
    if a.log:
        ax.set_yscale("log")
    ax.legend(fontsize=7, ncol=2)
    save(fig, a.dir, f"bars-{a.workload or 'all'}-{a.metric}-by-{a.by}{tag_of(a.where)}")


def curves(a, x_field, x_of, x_label, name):
    rows = select(read_summary(a.dir), a.workload, a.metric, a.where)
    one_per(rows, x_of, x_label)
    lines = defaultdict(list)
    for r in rows:
        x = x_of(r)
        if x is not None:
            lines[r["variant"]].append((x, num(r["median"]), num(r["lo"]), num(r["hi"]), bool(num(r.get("saturated")))))
    fig, ax = plt.subplots(figsize=(6.5, 4))
    for v, pts in sorted(lines.items()):
        pts.sort()
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        err = [[p[1] - p[2] for p in pts], [p[3] - p[1] for p in pts]]
        mk = MARKERS.get(v, "o")
        ax.errorbar(xs, ys, yerr=err, ls=family(v)[0], marker=mk, ms=4, capsize=2, color=colour(v), label=v)
        sat = [p for p in pts if p[4]]
        if sat:
            ax.plot([p[0] for p in sat], [p[1] for p in sat], ls="none", marker=mk, ms=4,
                    mfc="white", mec=colour(v), zorder=3)
    handles, labels = ax.get_legend_handles_labels()
    if any(p[4] for pts in lines.values() for p in pts):
        handles.append(Line2D([], [], ls="none", marker="o", ms=4, mfc="white", mec="#555555"))
        labels.append("saturated: a lower bound")
    ax.set_xlabel(x_label)
    ax.set_ylabel(LABELS.get(a.metric, a.metric))
    if a.log or a.metric.startswith("p"):
        ax.set_yscale("log")
    # Ticks where the measurements are, not at powers between them: rates on
    # a decimal log scale, thread counts on a binary one.
    ticks = sorted({p[0] for pts in lines.values() for p in pts})
    if x_field in ("interval_ns", "rate") or x_field in ("threads", "producers", "consumers") and len(ticks) > 1:
        binary = x_field not in ("interval_ns", "rate")
        ax.set_xscale("log", base=2 if binary else 10)
        ax.set_xticks(ticks)
        ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{x:g}") if binary else EngFormatter(sep=""))
        ax.xaxis.set_minor_formatter(NullFormatter())
        if len(ticks) > 6 and not binary:
            plt.setp(ax.get_xticklabels(), rotation=45, ha="right")
    ax.legend(handles, labels, fontsize=7, ncol=2)
    save(fig, a.dir, name)


def load(a):
    # Offered load in elements a second: from the interval for W2, the rate for W3.
    def x_of(r):
        iv, rate = num(r.get("interval_ns")), num(r.get("rate"))
        if iv:
            return 1e9 / iv
        return rate or None
    curves(a, "rate", x_of, "offered load (elements a second)",
           f"load-{a.workload or 'all'}-{a.metric}{tag_of(a.where)}")


def sweep(a):
    curves(a, a.x, lambda r: num(r.get(a.x)), a.x, f"sweep-{a.workload or 'all'}-{a.metric}-by-{a.x}{tag_of(a.where)}")


def cdf(a):
    pooled = defaultdict(lambda: defaultdict(int))
    with open(Path(a.dir) / "runs.jsonl") as f:
        for line in f:
            r = json.loads(line)
            if (a.workload and r.get("workload") != a.workload) or r.get("pass") or "hist_ns" not in r:
                continue
            if any(str(r.get(k, "")) != v for k, v in a.where.items()):
                continue
            for pair in r["hist_ns"].split(","):
                lo, n = pair.split(":")
                pooled[r["variant"]][float(lo)] += int(n)
    fig, ax = plt.subplots(figsize=(6.5, 4))
    for v, h in sorted(pooled.items()):
        xs = sorted(h)
        total = sum(h.values())
        acc, ys = 0, []
        for x in xs:
            acc += h[x]
            ys.append(acc / total)
        ax.step([max(x, 1.0) for x in xs], ys, where="post", ls=family(v)[0], color=colour(v), label=v)
    ax.set_xscale("log")
    ax.set_xlabel("latency (ns), bucket lower bound")
    ax.set_ylabel("fraction of elements")
    ax.legend(fontsize=7)
    save(fig, a.dir, f"cdf-{a.workload or 'all'}{tag_of(a.where)}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("kind", choices=["bars", "load", "sweep", "cdf"])
    p.add_argument("dir")
    p.add_argument("--workload", default="")
    p.add_argument("--metric", default="")
    p.add_argument("--by", default="elem")
    p.add_argument("--x", default="threads")
    p.add_argument("--log", action="store_true")
    p.add_argument("--where", action="append", default=[], help="field=value, repeatable")
    p.add_argument("--format", default="svg", choices=["svg", "pdf", "png"])
    a = p.parse_args()
    global FORMAT
    FORMAT = a.format
    a.where = dict(w.split("=", 1) for w in a.where)
    if not a.metric:
        a.metric = {"bars": "mops", "load": "p99", "sweep": "mops", "cdf": ""}[a.kind]
    {"bars": bars, "load": load, "sweep": sweep, "cdf": cdf}[a.kind](a)


if __name__ == "__main__":
    main()
