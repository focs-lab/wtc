#!/usr/bin/env python3
# Copy of the script the leg ran, with its paths written as $LEGS (the legs' working
# directory), $PROBES (the probes' material), $WTC_D4 / $WTC_D5 (worktrees at the
# measured commits), $WTC (this tree), $LEG_DIR (the host's leg directory).
"""D5 leg: CTPL's worker wait as released (orig), lifted by hand onto the eventcount
(probe Q5's ctpl_wait.py: hand-spin_then_park, hand-spin) and lifted by the
compiler (wtc, and wtcy<ns>: the same with the oversubscribed yield budget set to
<ns>; wtc is the 16 ms default).

    leg_d5.py <out-dir> <reps> [timed|untimed]

Programs: bench = benchmarks/apps/ctpl/bench.cpp (one submitter, the main thread);
multi = probe Q5's ctpl_bench_multi.cpp with 4 submitter threads. Arguments as in
the untimed sweep: WORKERS 1 s, 0 ns of work, INFLIGHT tasks in flight (4096, or 1
for ping-pong, where ns/task is a round trip). Cells:

  main (CPUs 4-23, 20 cores, as the untimed sweep's 20-core cpuset):
    bench and multi, W 2 4 8 16, inflight 4096; bench W 1 and 4, inflight 1
  oversubscribed (threads > processors of the mask; the lifted wait then spins with
  the oversubscribed yield budget, which the sweep variants vary):
    bench W 4 8 16 on CPUs 4-7; bench W 8 inflight 1 on 4-7; bench W 16 on 4-11;
    multi W 8 on 4-7 (producers not known to the compiler: its policy is
    spin_then_park whatever the budget, so only the four main variants run)

Each run is a fresh process, under the host's exclusive measurement lock; variants
rotated by repetition; two discarded warm-up runs and three control runs (orig,
bench W 4) before and after; orig-aa is orig's binary under a second name (A/A).
Each run records the busy share of the online processors outside 4-27.
"""
import json, os, re, resource, subprocess, sys, time

OUT, REPS = sys.argv[1], int(sys.argv[2])
MODE = sys.argv[3] if len(sys.argv) > 3 else "untimed"
H = os.path.dirname(os.path.abspath(__file__))
B = f"{H}/bin"
SET = set(range(4, 28))
SEC = "1"
MAIN_V = ["orig", "orig-aa", "hand-spin_then_park", "hand-spin", "wtc"]
SWEEP_V = ["orig", "hand-spin_then_park", "hand-spin", "wtc", "wtcy64000", "wtcy1000000", "wtcy4000000", "wtcy64000000"]
CELLS = []
for prog in ("bench", "multi"):
    for w in (2, 4, 8, 16):
        CELLS.append(dict(prog=prog, W=w, inflight=4096, cpus="4-23", over=0, vs=MAIN_V))
for w in (1, 4):
    CELLS.append(dict(prog="bench", W=w, inflight=1, cpus="4-23", over=0, vs=MAIN_V))
for w in (4, 8, 16):
    CELLS.append(dict(prog="bench", W=w, inflight=4096, cpus="4-7", over=1, vs=SWEEP_V))
CELLS.append(dict(prog="bench", W=8, inflight=1, cpus="4-7", over=1, vs=SWEEP_V))
CELLS.append(dict(prog="bench", W=16, inflight=4096, cpus="4-11", over=1, vs=SWEEP_V))
CELLS.append(dict(prog="multi", W=8, inflight=4096, cpus="4-7", over=1, vs=MAIN_V[:1] + MAIN_V[2:]))

def stat():
    d = {}
    for l in open("/proc/stat"):
        if l.startswith("cpu") and l[3].isdigit():
            f = l.split(); d[int(f[0][3:])] = list(map(int, f[1:]))
    return d

def busy(a, b):
    out = []
    for c, v in b.items():
        if c in SET or c not in a: continue
        dd = [x - y for x, y in zip(v, a[c])]; t = sum(dd)
        out.append((t - dd[3] - dd[4]) / t if t else 0.0)
    return (sum(out) / len(out) if out else 0.0), (max(out) if out else 0.0)

def lock(cmd):
    return ["machine-lock", "--lane", "wtc-bench", "--measure", "--minutes", "5", "--why", "wtc d5 leg", "--no-scope", "--"] + cmd

os.makedirs(OUT, exist_ok=True)
RUNS = open(f"{OUT}/runs.jsonl", "a")
LOG = open(f"{OUT}/log.txt", "a")

def run(tag, rep, c, var):
    exe = f"{B}/{c['prog']}-{'orig' if var == 'orig-aa' else var}"
    a = [exe, str(c["W"]), SEC, "0", str(c["inflight"])] + (["4"] if c["prog"] == "multi" else [])
    cmd = lock(["timeout", "120", "taskset", "-c", c["cpus"]] + a)
    s0 = stat(); r0 = resource.getrusage(resource.RUSAGE_CHILDREN); t0 = time.time()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace")
    wall = time.time() - t0; r1 = resource.getrusage(resource.RUSAGE_CHILDREN); s1 = stat()
    bm, bx = busy(s0, s1)
    g = lambda k: (lambda mm: float(mm.group(1)) if mm else None)(re.search(rf"{re.escape(k)}=([\d.]+)", p.stdout))
    r = dict(leg="d5", mode=MODE, tag=tag, rep=rep, prog=c["prog"], S=(4 if c["prog"] == "multi" else 1), W=c["W"],
             inflight=c["inflight"], cpus=c["cpus"], over=c["over"], var=var, rc=p.returncode,
             ns_task=g("ns/task"), mtasks_s=g("Mtasks/s"), cpu_ns_task=g("cpu_ns/task"), sys_pct=g("sys%"),
             vcsw_task=g("vcsw/task"), bad=g("bad"), wall_s=round(wall, 4),
             child_usr_s=round(r1.ru_utime - r0.ru_utime, 4), child_sys_s=round(r1.ru_stime - r0.ru_stime, 4),
             busy_out_mean=round(bm, 4), busy_out_max=round(bx, 4),
             freq_khz=open("/sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq").read().strip(),
             time=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    RUNS.write(json.dumps(r) + "\n"); RUNS.flush()
    LOG.write(f"[{r['time']}] {tag} rep={rep} {c['prog']} W={c['W']} if={c['inflight']} cpus={c['cpus']} {var} "
              f"rc={p.returncode} Mt/s={r['mtasks_s']} busy_out={bm:.3f}\n")
    if p.returncode != 0 or r["mtasks_s"] is None or r["bad"]:
        LOG.write((p.stdout + p.stderr)[-2000:] + "\n")
    LOG.flush()

CTRL = dict(prog="bench", W=4, inflight=4096, cpus="4-23", over=0)
LOG.write(f"=== d5 leg {MODE} reps={REPS} start {time.strftime('%FT%TZ', time.gmtime())} load {open('/proc/loadavg').read().split()[:3]}\n")
if MODE == "timed":
    e = subprocess.run(["$WTC_D5/benchmarks/scripts/check_env.sh", "24"], capture_output=True, text=True)
    LOG.write(e.stdout); LOG.flush()
    if e.returncode != 0: sys.exit("environment not acceptable")
for k in range(2): run("warmup", -1, CTRL, "orig")
for k in range(3): run("control-start", 900, CTRL, "orig")
for rep in range(REPS):
    for c in CELLS:
        vs = c["vs"]
        for k in range(len(vs)):
            run("timed" if MODE == "timed" else "untimed", rep, c, vs[(k + rep) % len(vs)])
for k in range(3): run("control-end", 901, CTRL, "orig")
LOG.write(f"=== end {time.strftime('%FT%TZ', time.gmtime())}\n")
