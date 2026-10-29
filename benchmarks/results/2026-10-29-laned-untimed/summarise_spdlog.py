# Tables of SUMMARY.md from spdlog-runs.jsonl: python3 summarise_spdlog.py spdlog-runs.jsonl
import json,sys,statistics as st
from collections import defaultdict
g=defaultdict(list)
for l in open(sys.argv[1]):
    r=json.loads(l); g[(r["payload"],r["threads"],r["variant"])].append(r)
V=["orig","compiler","hand","hand-cxx","laned-cxx"]
for p in [8,64]:
    print(f"\n### payload {p} B: M messages/s median [min–max]; log-call p50 / p99 ns\n")
    print("| threads | "+" | ".join(V)+" | laned / hand-cxx | laned / compiler |"); print("|---|"+"---|"*(len(V)+2))
    for t in [1,2,4,8]:
        cs=[]
        for v in V:
            rs=g[(p,t,v)]; m=[r["mops"] for r in rs]
            cs.append("{:.2f} [{:.2f}–{:.2f}]; {:.0f} / {:.0f}".format(st.median(m),min(m),max(m),st.median([r["p50"] for r in rs]),st.median([r["p99"] for r in rs])))
        md=lambda v: st.median([r["mops"] for r in g[(p,t,v)]])
        print(f"| {t} | "+" | ".join(cs)+f" | {md('laned-cxx')/md('hand-cxx'):.2f}× | {md('laned-cxx')/md('compiler'):.2f}× |")
print("\nruns per cell:", sorted(set(len(x) for x in g.values())))
