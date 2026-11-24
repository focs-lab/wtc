#!/usr/bin/env python3
# Copy of the script the leg ran, with its paths written as $LEGS (the legs' working
# directory), $PROBES (the probes' material), $WTC_D4A (the worktree at the measured
# commit), $LEG_DIR (the host's leg directory).
"""D4 leg: LevelDB readrandom, the DB mutex as written, under LiTL locks, delegated
by hand (probe Q4) and delegated by the compiler (wtc-cc --delegate).
The adaptive delegation's leg (leg_d4a.py) is this harness with two arms added:
adapt (the adaptive runtime's build, as it runs by default) and deleg-always (the
same binary with WTC_DELEGATE_MODE=always), and the clients' counts of where their
sections ran read from the report; adapt-h<N> sets WTC_DELEGATE_HOT_NS=N (probes).

    leg_d4a.py <out-dir> <reps> [timed|untimed]

Runs on CPUs 4-27 (in a timed leg: a bench reservation of 24 cores, SMT siblings
offline). Every run is a fresh process on a fresh copy of one pristine database
(fillrandom, 1M keys, 100-byte values, built once at the start), so no run sees
another's compactions. For T client threads every arm gets the same mask: CPUs
4..3+T plus CPU 27, where the delegation servers are pinned (WTC_DELEGATE_CPU /
WTC_DELEG_CPU); deleg-float leaves the compiler's server unpinned in that mask.
Reads per thread are 3M/T, so a run reads 3M keys whatever T is.

Discipline as in benchmarks/scripts/campaign.sh: two discarded warm-up runs, three
control runs (orig, 8 threads) before and after, arms rotated by repetition, each
run under the host's exclusive measurement lock, orig-aa the same binary as orig
under a second name (the A/A check). Each run records the busy share of the
online processors outside 4-27 over its own length (the disturbance gate reads it).
"""
import json, os, re, shutil, subprocess, sys, time

OUT, REPS = sys.argv[1], int(sys.argv[2])
MODE = sys.argv[3] if len(sys.argv) > 3 else "untimed"
H = os.path.dirname(os.path.abspath(__file__))
B = f"{H}/bin"
SHM = "/dev/shm/wtc-legs-d4a"
PRISTINE, DB = f"{SHM}/pristine", f"{SHM}/db"
SET = set(range(4, 28))
SERVER_CPU = 27
THREADS = [int(x) for x in os.environ.get("D4_THREADS", "1 2 4 8 12 16 23").split()]
TOTAL_READS = int(os.environ.get("D4_TOTAL_READS", "3000000"))
ARMS = os.environ.get("D4_ARMS", "orig orig-aa wtc deleg deleg-always adapt litl-mcs").split()
LITL = {"litl-mcs": "mcs_spinlock", "litl-clh": "clh_spinlock", "litl-mcsstp": "mcs_spin_then_park"}

def arm_cmd(arm):
    env = {}
    if arm in ("orig", "orig-aa") or arm in LITL:
        b = f"{B}/db_bench-leveldb-orig"
        if arm in LITL: env["LD_PRELOAD"] = f"{B}/litl/lib{LITL[arm]}.so"
    elif arm == "wtc":
        b = f"{B}/db_bench-wtc"
    elif arm == "deleg":
        b = f"{B}/db_bench-deleg"; env.update(WTC_DELEGATE_CPU=str(SERVER_CPU), WTC_DELEGATE_REPORT="1")
    elif arm == "deleg-float":
        b = f"{B}/db_bench-deleg"; env.update(WTC_DELEGATE_REPORT="1")
    elif arm == "deleg-always":
        b = f"{B}/db_bench-adapt"; env.update(WTC_DELEGATE_MODE="always", WTC_DELEGATE_CPU=str(SERVER_CPU), WTC_DELEGATE_REPORT="1")
    elif arm == "adapt" or arm.startswith("adapt-h"):
        b = f"{B}/db_bench-adapt"; env.update(WTC_DELEGATE_CPU=str(SERVER_CPU), WTC_DELEGATE_REPORT="1")
        if arm != "adapt": env["WTC_DELEGATE_HOT_NS"] = arm[len("adapt-h"):]
    elif arm == "hand":
        b = f"{B}/db_bench-leveldb-deleg"; env.update(WTC_DELEG_CPU=str(SERVER_CPU))
    else:
        raise SystemExit(f"unknown arm {arm}")
    return b, env

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

def mask(t):
    return f"4-{3 + t},{SERVER_CPU}" if t > 1 else f"4,{SERVER_CPU}"

def lock(cmd):
    return ["machine-lock", "--lane", "wtc-bench", "--measure", "--minutes", "5", "--why", "wtc d4a leg", "--no-scope", "--"] + cmd

os.makedirs(OUT, exist_ok=True)
RUNS = open(f"{OUT}/runs.jsonl", "a")
LOG = open(f"{OUT}/log.txt", "a")

def run(tag, rep, t, arm):
    b, env = arm_cmd(arm)
    reads = TOTAL_READS // t
    shutil.rmtree(DB, ignore_errors=True)
    shutil.copytree(PRISTINE, DB)
    cmd = lock(["env"] + [f"{k}={v}" for k, v in env.items()] +
               ["timeout", "300", "taskset", "-c", mask(t), f"{B}/runus", b, "--benchmarks=readrandom",
                "--use_existing_db=1", "--num=1000000", f"--reads={reads}", f"--threads={t}", f"--db={DB}"])
    s0 = stat(); t0 = time.time()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    wall = time.time() - t0; s1 = stat()
    o = p.stdout.replace("\r", "\n")
    bm, bx = busy(s0, s1)
    m = re.search(r"readrandom\s*:\s*([\d.]+) micros/op", o)
    us = float(m.group(1)) if m else 0.0
    v = lambda k: (lambda mm: float(mm.group(1)) if mm else None)(re.search(rf"{k}=([\d.]+)", o))
    served = sum(int(x) for x in re.findall(r"ran (\d+) sections", o)) or None
    found = re.search(r"\((\d+) of (\d+) found\)", o)
    where = re.search(r"clients: \d+ sections, free=(\d+) contended=(\d+) delegated=(\d+)", o)
    r = dict(leg="d4a", mode=MODE, tag=tag, rep=rep, threads=t, arm=arm, mask=mask(t), reads_per_thread=reads,
             rc=p.returncode, micros_op=us, ops_s=(t * 1e6 / us if us else 0.0), wall_s=round(wall, 4),
             usr_s=v("usr_s"), sys_s=v("sys_s"), ctxsw=v("ctxsw"), maxrss_kb=v("maxrss_kb"), served=served,
             where=([int(x) for x in where.groups()] if where else None),
             found=(f"{found.group(1)}/{found.group(2)}" if found else None),
             busy_out_mean=round(bm, 4), busy_out_max=round(bx, 4),
             freq_khz=open("/sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq").read().strip(),
             time=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    RUNS.write(json.dumps(r) + "\n"); RUNS.flush()
    LOG.write(f"[{r['time']}] {tag} rep={rep} T={t} {arm} rc={p.returncode} ops/s={r['ops_s']:.0f} busy_out={bm:.3f}\n")
    if p.returncode != 0 or not us:
        LOG.write(o[-2000:] + "\n")
    LOG.flush()
    return r

LOG.write(f"=== d4a leg {MODE} reps={REPS} start {time.strftime('%FT%TZ', time.gmtime())} load {open('/proc/loadavg').read().split()[:3]}\n")
if MODE == "timed":
    e = subprocess.run(["$WTC_D4A/benchmarks/scripts/check_env.sh", "24"], capture_output=True, text=True)
    LOG.write(e.stdout); LOG.flush()
    if e.returncode != 0: sys.exit("environment not acceptable")
shutil.rmtree(SHM, ignore_errors=True); os.makedirs(SHM)
subprocess.run(["taskset", "-c", "4-27", f"{B}/db_bench-leveldb-orig", "--benchmarks=fillrandom", "--num=1000000",
                "--value_size=100", "--threads=1", f"--db={PRISTINE}"], check=True, stdout=subprocess.DEVNULL)
LOG.write(f"pristine db: {sum(os.path.getsize(os.path.join(PRISTINE, f)) for f in os.listdir(PRISTINE))} bytes, "
          f"{len(os.listdir(PRISTINE))} files\n")
for k in range(2): run("warmup", -1, 8, "orig")
for k in range(3): run("control-start", 900, 8, "orig")
for rep in range(REPS):
    for t in THREADS:
        for k in range(len(ARMS)):
            run("timed" if MODE == "timed" else "untimed", rep, t, ARMS[(k + rep) % len(ARMS)])
for k in range(3): run("control-end", 901, 8, "orig")
shutil.rmtree(SHM, ignore_errors=True)
LOG.write(f"=== end {time.strftime('%FT%TZ', time.gmtime())}\n")
