#!/usr/bin/env python3
"""The table of benchmarks/scripts/lift_attribution.sh: per program and
configuration, the median of each variant over its runs (and the range),
and the compiler's choice against the general-purpose queue, both lifted
from the same source.

    lift_attribution.py <raw.csv>

A rate (Mmsg/s, Mtasks/s) is better higher, a time (s) lower; the ratio
compiler/generic is written so that above one is the compiler's gain
either way.
"""
import csv
import statistics
import sys
from collections import OrderedDict, defaultdict

VARIANTS = ["orig", "generic", "compiler", "port", "port-mpsc"]


def main():
    runs = defaultdict(list)
    units = {}
    order = OrderedDict()
    for row in csv.DictReader(open(sys.argv[1])):
        key = (row["program"], row["config"])
        order.setdefault(row["program"], OrderedDict())[row["config"]] = True
        units[row["program"]] = row["unit"]
        try:
            runs[key + (row["variant"],)].append(float(row["value"]))
        except ValueError:
            runs[key + (row["variant"],)].append(float("nan"))
    print("# Lifted programs: what the compiler's choice adds")
    print()
    print("Median of the runs (min-max). `generic`: the program's own queue class lifted onto")
    print("the general-purpose queue (wtc-cc --no-specialize); `compiler`: lifted, with the")
    print("compiler's choice; `port`: the hand port onto wtc::queue, through wtc-cc.")
    print("compiler/generic above 1 is the compiler's gain.")
    for prog, configs in order.items():
        unit = units[prog]
        present = [v for v in VARIANTS if any((prog, c, v) in runs for c in configs)]
        print()
        print(f"## {prog} ({unit}, {'lower' if unit == 's' else 'higher'} is better)")
        print()
        print("| config | " + " | ".join(present) + " | compiler/generic |")
        print("|---|" + "---|" * len(present) + "---|")
        for c in configs:
            cells, med = [], {}
            for v in present:
                xs = [x for x in runs.get((prog, c, v), []) if x == x]
                if not xs:
                    cells.append("")
                    continue
                m = statistics.median(xs)
                med[v] = m
                cells.append(f"{m:.3g} ({min(xs):.3g}-{max(xs):.3g})")
            ratio = ""
            if "compiler" in med and "generic" in med and med["generic"] and med["compiler"]:
                r = med["generic"] / med["compiler"] if unit == "s" else med["compiler"] / med["generic"]
                ratio = f"**{r:.2f}x**"
            print(f"| {c} | " + " | ".join(cells) + f" | {ratio} |")


if __name__ == "__main__":
    main()
