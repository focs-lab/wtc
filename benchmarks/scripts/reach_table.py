#!/usr/bin/env python3
"""The evaluation's table of reach and precision (RQ1): every queue of every
program it compiles, the analysis's verdict beside the topology read from
the source by hand (benchmarks/reach/oracle.json), and what came of it.

    reach_table.py <oracle.json> <logs-dir>

<logs-dir> holds each build's wtc-cc diagnostics, named as the oracle
names them (benchmarks/scripts/reach.sh collects them). A verdict is

  exact         the analysis's producers and consumers are the oracle's;
  conservative  it counts many where the oracle has one: sound, and
                specialises less than it could;
  refused       the queue is unknown to it, or it gave no verdict;
  WRONG         it counts one where the oracle has many: a claim that
                would specialise a queue unsafely. There must be none, and
                the script exits non-zero if there is.

The cause column is the oracle's: what in the program stands between the
analysis and the oracle, as the builds that remove it have shown. The last
column is the analysis's own account where it gave one: why it counted
many on a side, or the reason of an unresolvable access in the same build.
"""
import json
import re
import sys
from collections import Counter
from pathlib import Path

VERDICT = re.compile(r"wtc: queue (\S+): (spsc|spmc|mpsc|mpmc|unknown) \(producers: (\w+), consumers: (\w+)\)")
UNRESOLVED = re.compile(r"wtc: queue access is unresolvable \(([^)]*)\)")
WHY_MANY = re.compile(r"wtc: queue (\S+): many (producers|consumers): (.*)$", re.M)


def side(n):
    return "1" if n in ("0", "1") else "many"


def topology(p, c):
    return {("1", "1"): "spsc", ("many", "1"): "mpsc", ("1", "many"): "spmc"}.get((p, c), "mpmc")


def label_matches(label, name):
    # @q, or a mangled name ending in the queue's: @_ZL10refine_que, @_ZN..6stage1E
    return re.search(rf"(^@|\d){re.escape(name)}E?$", label) is not None


def main():
    oracle = json.load(open(sys.argv[1]))
    logs = Path(sys.argv[2])
    counts = Counter()
    rows = []
    for prog in oracle["programs"]:
        path = logs / prog["log"]
        text = path.read_text(errors="replace") if path.exists() else None
        verdicts = VERDICT.findall(text or "")
        reasons = sorted(set(UNRESOLVED.findall(text or "")))
        why_many = WHY_MANY.findall(text or "")
        for q in prog["queues"]:
            o = topology(q["producers"], q["consumers"])
            found = [v for v in verdicts if label_matches(v[0], q["queue"])]
            if text is None:
                outcome, said = "no build", "-"
            elif not found:
                outcome, said = "refused", "no verdict"
            else:
                _, t, p, c = found[0]
                p, c = side(p), side(c)
                said = f"{t} ({p}, {c})" if t != "unknown" else "unknown"
                if t == "unknown":
                    outcome = "refused"
                elif (p == "1" and q["producers"] == "many") or (c == "1" and q["consumers"] == "many"):
                    outcome = "WRONG"
                elif (p, c) == (q["producers"], q["consumers"]):
                    outcome = "exact"
                else:
                    outcome = "conservative"
            counts[outcome] += 1
            if outcome == "exact" and o == "spsc":
                counts["exact spsc"] += 1
            if o == "spsc":
                counts["oracle spsc"] += 1
            account = ""
            if outcome == "conservative":
                account = "; ".join(f"{side}: {why}" for label, side, why in why_many
                                    if label_matches(label, q["queue"]) and q[side] == "1")
            elif outcome == "refused":
                account = "; ".join(reasons)
            rows.append((prog["program"], q["queue"], f"{o} ({q['producers']}, {q['consumers']})", said, outcome,
                         q.get("cause", "") if outcome != "exact" else "", account))

    print("| program | queue | oracle | verdict | outcome | cause | the analysis's account |")
    print("|---|---|---|---|---|---|---|")
    for r in rows:
        print("| " + " | ".join(r) + " |")
    total = sum(counts[k] for k in ("exact", "conservative", "refused", "WRONG", "no build"))
    print(f"\n{total} queues: {counts['exact']} exact ({counts['exact spsc']} of the "
          f"{counts['oracle spsc']} single-producer single-consumer ones proved so), "
          f"{counts['conservative']} conservative, {counts['refused']} refused, "
          f"{counts['WRONG']} wrong" + (f", {counts['no build']} without a build log" if counts["no build"] else "") + ".")
    sys.exit(1 if counts["WRONG"] else 0)


if __name__ == "__main__":
    main()
