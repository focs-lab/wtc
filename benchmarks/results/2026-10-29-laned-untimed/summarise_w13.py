# Tables of SUMMARY.md from w13-runs.jsonl: python3 summarise_w13.py w13-runs.jsonl
import json, sys, statistics as st
from collections import defaultdict
rows=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
V=["shim_laned_move","laned_noslip","shim_mpsc_move","shim_generic_move","moodycamel_cq_inline","std_mutex_inline"]
short={"shim_laned_move":"laned","laned_noslip":"laned, no slip","shim_mpsc_move":"seg MPSC","shim_generic_move":"seg MPMC","moodycamel_cq_inline":"moodycamel CQ","std_mutex_inline":"mutex+deque"}
g=defaultdict(list)
for r in rows:
    if r["mode"]=="stream": k=("stream",r["elem"],r["producers"])
    elif r.get("sleep")=="yes": k=("sparse",r["producers"],r["burst"])
    else: k=("paced",r["producers"])
    g[k+(r["variant"],)].append(r)
med=st.median
def cell(rs,f):
    xs=[r[f] for r in rs if f in r]
    if not xs: return "-"
    return "{:.1f} [{:.1f}–{:.1f}]".format(med(xs),min(xs),max(xs))
print("## Stream throughput, M elements/s: median [min–max] of 10 runs\n")
for e in ["8","64","256"]:
    print(f"\n### {e}-byte elements\n")
    print("| P | "+" | ".join(short[v] for v in V)+" | laned / seg MPSC | laned / moodycamel |")
    print("|---|"+"---|"*(len(V)+2))
    for P in [1,2,4,8,16]:
        cells=[cell(g[("stream",e,P,v)],"mops") for v in V]
        m=lambda v: med([r["mops"] for r in g[("stream",e,P,v)]])
        print(f"| {P} | "+" | ".join(cells)+f" | {m('shim_laned_move')/m('shim_mpsc_move'):.2f}× | {m('shim_laned_move')/m('moodycamel_cq_inline'):.2f}× |")
def tri(rs,fs,fmt="{:.0f}"):
    return " / ".join(fmt.format(med([r[f] for r in rs])) for f in fs) if rs else "-"
for title,fs in [("push p50 / pop p50 / empty poll p50",["push_p50","pop_p50","empty_p50"]),("push p99 / pop p99",["push_p99","pop_p99"])]:
    print(f"\n## Stream, 64-byte elements, per operation: {title}, ns (median over runs)\n")
    print("| P | "+" | ".join(short[v] for v in V)+" |"); print("|---|"+"---|"*len(V))
    for P in [1,2,4,8,16]:
        print(f"| {P} | "+" | ".join(tri(g[("stream","64",P,v)],fs) for v in V)+" |")
print("\n## Paced: 1 M elements/s in total over P spinning producers, 64-byte elements; push-to-pop latency p50 / p99 / p99.9, ns (median over runs)\n")
print("| P | "+" | ".join(short[v] for v in V)+" |"); print("|---|"+"---|"*len(V))
for P in [1,2,4,8,16]:
    print(f"| {P} | "+" | ".join(tri(g[("paced",P,v)],["lat_p50","lat_p99","lat_p999"]) for v in V)+" |")
print("\n## Sparse and bursty: sleeping producers on 38 hardware threads, 64-byte elements; latency p50 / p99 / p99.9 ns, then empty-poll p50 ns, then delivered M/s\n")
print("| case | "+" | ".join(short[v] for v in V)+" |"); print("|---|"+"---|"*len(V))
for P,b,name in [(16,1,"16 producers, 1 per 200 us"),(64,1,"64 producers, 1 per 200 us"),(192,1,"192 producers, 1 per 200 us"),(64,32,"64 producers, 32 per 2 ms")]:
    cs=[]
    for v in V:
        rs=g[("sparse",P,b,v)]
        cs.append(tri(rs,["lat_p50","lat_p99","lat_p999"])+"; "+tri(rs,["empty_p50"])+"; "+tri(rs,["mops"],"{:.2f}") if rs else "-")
    print(f"| {name} | "+" | ".join(cs)+" |")
print("\nruns per cell:", sorted(set(len(x) for x in g.values())))
print("\npeak RSS MiB, stream E64 P=16:", {short[v]: round(med([r['peak_rss_kib'] for r in g[('stream','64',16,v)]])/1024) for v in V})
print("peak RSS MiB, sparse P=192:", {short[v]: round(med([r['peak_rss_kib'] for r in g[('sparse',192,1,v)]])/1024) for v in V})
