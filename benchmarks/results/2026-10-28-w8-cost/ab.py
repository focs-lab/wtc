"""Paired ratios for the cost model's check on W8:

    python3 ab.py runs.jsonl decisions.txt [monitor.log]

Each run is one of three builds of one level of work (NOTE.md): plain (no
batching), off (batched wherever legal: the model off) and on (the model's
decision). Where the model batched, on and off are the same program, and
where it refused, on and plain are; so each repetition's batched and
plain rates are the geometric means of the runs of their program, and a
ratio is taken within a repetition. Printed per configuration: the model's
decision for the measured builds (the first section of decisions.txt), the
median of batched / plain over the repetitions with its range and the
number of repetitions, what the model's choice made of that (batched /
plain where it batched, 1 where it refused), and on / off and on / plain
from the on runs alone. Rates are millions of sections a second over all
threads; work_ns is the median of what the level's rounds took on its runs.

With monitor.log (every 5 s, the time and whatever else held the leg's
node), a run during which, or within 5 s of which, anything else held the
node is left out, and with it that configuration's other runs of the same
repetition; the count of repetitions left out is printed.
"""
import json, math, re, statistics as st, sys
from collections import defaultdict
from datetime import datetime, timedelta


def when(text):
    return datetime.strptime(text.rstrip("Z")[:23], "%Y-%m-%dT%H:%M:%S.%f")


busy = []
if len(sys.argv) > 3:
    for line in open(sys.argv[3]):
        parts = line.split()
        if len(parts) > 1:
            busy.append(when(parts[0]))
slack = timedelta(seconds=5)
disturbed = set()   # (threads, level, rep)

runs = defaultdict(lambda: defaultdict(dict))   # (threads, level) -> rep -> flavour -> mops
work = defaultdict(list)
for line in open(sys.argv[1]):
    r = json.loads(line)
    cfg = (r["threads"], int(r["work"].rstrip("ns")))
    runs[cfg][r["rep"]][r["variant"]] = r["mops"]
    if busy and "utc_start" in r:
        a, b = when(r["utc_start"]) - slack, when(r["utc_end"]) + slack
        if any(a <= t <= b for t in busy):
            disturbed.add(cfg + (r["rep"],))
    work[cfg].append(r["work_ns"])

batched = {}
for line in open(sys.argv[2]):
    if line.startswith("==") and batched:
        break
    m = re.match(r"(\d+) ns, \d+ rounds: (?:critical sections in this loop )?(batched|not batched)", line)
    if m:
        batched[int(m.group(1))] = m.group(2) == "batched"


def gm(v):
    return math.exp(sum(map(math.log, v)) / len(v))


def summary(p):
    return f"{st.median(p):5.2f} ({min(p):.2f}-{max(p):.2f})"


print("threads level work_ns model        batched/plain        model/plain  on/off              on/plain            n")
for cfg in sorted(runs):
    t, l = cfg
    yes = batched[l]
    bp, onoff, onplain = [], [], []
    for rep, f in runs[cfg].items():
        if not {"plain", "off", "on"} <= f.keys() or cfg + (rep,) in disturbed:
            continue
        b = gm([f["off"], f["on"]] if yes else [f["off"]])
        p = gm([f["plain"]] if yes else [f["plain"], f["on"]])
        bp.append(b / p)
        onoff.append(f["on"] / f["off"])
        onplain.append(f["on"] / f["plain"])
    model = st.median(bp) if yes else 1.0
    print(f"{t:7} {l:5} {st.median(work[cfg]):7.0f} {'batched' if yes else 'refused':12} {summary(bp)}  "
          f"{model:5.2f}        {summary(onoff)}  {summary(onplain)}  {len(bp)}")
if len(sys.argv) > 3:
    print(f"left out: {len(disturbed)} configuration-repetitions during which something else held the node")
