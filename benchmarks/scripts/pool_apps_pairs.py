#!/usr/bin/env python3
"""Paired ratios for the thread-pool libraries' leg (pool_apps_leg.sh).

    pool_apps_pairs.py <runs.jsonl> <out-dir>

A ratio is taken within a round, between two builds of the same program
in the same configuration (workers, work a task), from runs made seconds
apart; the median over the rounds stands for the configuration; and a
library's figure is the geometric mean of those medians over its
configurations, with their smallest and largest. Writes SUMMARY.md (the
tables) and pairs.csv (every configuration's median of every ratio, and
the median rate of every build).
"""
import collections
import csv
import json
import math
import os
import statistics
import sys

RATIOS = [
    # name, numerator, denominator, what it is
    ("topology", "compiler", "generic", "the compiler's choice over the port on the general-purpose queue"),
    ("hand", "hand", "compiler", "the queue declared by hand over the compiler's choice"),
    ("port", "generic", "orig", "the port on the general-purpose queue over the library as released"),
    ("total", "compiler", "orig", "the compiler's build over the library as released"),
    ("sleeping", "orig-spin", "orig", "the control (workers spin) over the library as released (workers sleep)"),
    ("queue", "generic", "orig-spin", "the port's lock-free queue over the library's locked queue, both spinning"),
    ("clangir", "orig", "orig-clang", "the library through wtc-cc over the library through clang++-18"),
]


def geomean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def main():
    runs, out = sys.argv[1], sys.argv[2]
    rates = collections.defaultdict(dict)   # (lib, subs, workers, work, rep) -> build -> mops
    for line in open(runs):
        r = json.loads(line)
        if r.get("check") != "ok":
            continue
        key = (r["workload"], int(r["submitters"]), int(r["workers"]), int(r["work_ns"]), int(r["rep"]))
        rates[key][r["variant"]] = float(r["mops"])

    # Per configuration: each ratio's per-round values, and each build's rates.
    per_cfg = collections.defaultdict(lambda: collections.defaultdict(list))
    rate_cfg = collections.defaultdict(lambda: collections.defaultdict(list))
    for (lib, subs, w, work, rep), b in rates.items():
        cfg = (lib, subs, w, work)
        for name, num, den, _ in RATIOS:
            if num in b and den in b:
                per_cfg[cfg][name].append(b[num] / b[den])
        for build, v in b.items():
            rate_cfg[cfg][build].append(v)

    med = {cfg: {n: statistics.median(v) for n, v in d.items()} for cfg, d in per_cfg.items()}
    reps = {cfg: max(len(v) for v in d.values()) for cfg, d in per_cfg.items()}
    groups = sorted({(c[0], c[1]) for c in med})

    lines = ["# Thread-pool libraries: paired ratios", ""]
    lines.append("Ratios of tasks a second, taken within a round between two builds of one "
                 "program in one configuration; the median over the rounds per configuration; "
                 "the geometric mean over configurations, with the smallest and largest median.")
    lines.append("")
    for name, num, den, what in RATIOS:
        lines.append(f"- **{name}**, {num} / {den}: {what}")
    lines.append("")
    for lib, subs in groups:
        cfgs = sorted(c for c in med if c[0] == lib and c[1] == subs)
        works = sorted({c[3] for c in cfgs})
        workers = sorted({c[2] for c in cfgs})
        lines.append(f"## {lib}, {subs} submitter{'s' if subs > 1 else ''}")
        lines.append("")
        lines.append(f"{len(cfgs)} configurations, {min(reps[c] for c in cfgs)}-{max(reps[c] for c in cfgs)} rounds each.")
        lines.append("")
        lines.append("| ratio | all | " + " | ".join(f"{w} ns" for w in works) + " |")
        lines.append("|---|---|" + "---|" * len(works))
        for name, *_ in RATIOS:
            vals = [med[c][name] for c in cfgs if name in med[c]]
            if not vals:
                continue
            row = f"| {name} | {geomean(vals):.2f} ({min(vals):.2f}-{max(vals):.2f}) |"
            for wk in works:
                v = [med[c][name] for c in cfgs if c[3] == wk and name in med[c]]
                row += f" {geomean(v):.2f} ({min(v):.2f}-{max(v):.2f}) |"
            lines.append(row)
        lines.append("")
        for name in ("topology", "total"):
            lines.append(f"{name} by workers (rows) and work a task (columns), medians over the rounds:")
            lines.append("")
            lines.append("| workers | " + " | ".join(f"{w} ns" for w in works) + " |")
            lines.append("|---|" + "---|" * len(works))
            for wr in workers:
                row = f"| {wr} |"
                for wk in works:
                    v = med.get((lib, subs, wr, wk), {}).get(name)
                    row += f" {v:.2f} |" if v is not None else " - |"
                lines.append(row)
            lines.append("")
        builds = sorted({b for c in cfgs for b in rate_cfg[c]})
        lines.append("Median M tasks a second:")
        lines.append("")
        lines.append("| workers | work ns | " + " | ".join(builds) + " |")
        lines.append("|---|---|" + "---|" * len(builds))
        for c in cfgs:
            row = f"| {c[2]} | {c[3]} |"
            for b in builds:
                v = rate_cfg[c].get(b)
                row += f" {statistics.median(v):.3f} |" if v else " - |"
            lines.append(row)
        lines.append("")

    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "SUMMARY.md"), "w") as f:
        f.write("\n".join(lines))
    with open(os.path.join(out, "pairs.csv"), "w", newline="") as f:
        wr = csv.writer(f, lineterminator="\n")
        builds = sorted({b for c in rate_cfg for b in rate_cfg[c]})
        wr.writerow(["library", "submitters", "workers", "work_ns", "rounds"] + [n for n, *_ in RATIOS]
                    + [f"mops_{b}" for b in builds])
        for c in sorted(med):
            wr.writerow(list(c) + [reps[c]]
                        + [f"{med[c][n]:.4f}" if n in med[c] else "" for n, *_ in RATIOS]
                        + [f"{statistics.median(rate_cfg[c][b]):.4f}" if rate_cfg[c].get(b) else "" for b in builds])
    print("\n".join(lines))


if __name__ == "__main__":
    main()
