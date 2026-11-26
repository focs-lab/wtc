#!/usr/bin/env python3
"""The progress runtime's queue leg: main against the progress runtime's
head on the queue microbenchmarks that run through the runtime's queues, and
the two new rings of indices against the bounded ring.

    leg_pr72.py <out-dir> <reps> [timed|untimed]

Environment:
    PR72_BIN    directory holding one build per arm: main/, main-aa/ (a copy
                of main/, the A/A arm), pr/ (build.sh's output for each)
    PR72_SET    processors the runs get (taskset -c), e.g. 4-27
    PR72_LOCK   untimed: a file each run holds with flock; timed: each run
                takes machine-lock --measure instead
    PR72_CELLS  optional: only the cells whose key contains one of these
                space-separated words

Every run is a fresh process; arms are rotated by repetition inside each
cell; two discarded warm-up runs and three control runs before and after
(W1 shim_generic E64, main), as benchmarks/scripts/campaign.sh does. Each
run records the busy share of the online processors outside PR72_SET over
its own length (the disturbance gate reads it). In a timed leg the
environment is checked first (benchmarks/scripts/check_env.sh of the
measured tree, PR72_CHECK_ENV).
"""
import json, os, subprocess, sys, time

OUT, REPS = sys.argv[1], int(sys.argv[2])
MODE = sys.argv[3] if len(sys.argv) > 3 else "untimed"
BIN = os.environ["PR72_BIN"]
SET = os.environ["PR72_SET"]
LOCKF = os.environ.get("PR72_LOCK", "")
# PR72_LOCK_SCOPE=rep: hold PR72_LOCK for a whole repetition (and for the
# warm-up and controls) instead of each run, so that runs do not each queue
# behind another user of the lock. PR72_REP0: the first repetition's index,
# for a leg continued after an interruption.
# PR72_LOCK_SCOPE=leg: hold it from the warm-up to the last control, for a
# leg of a few minutes.
SCOPE = os.environ.get("PR72_LOCK_SCOPE", "run")
REP_SCOPE = SCOPE in ("rep", "leg")
REP0 = int(os.environ.get("PR72_REP0", "0"))
ONLY = os.environ.get("PR72_CELLS", "").split()
AB = ["main", "main-aa", "pr"]
# A bisect names its own arms (builds under PR72_BIN, the first one also
# running the warm-up and the controls) and exact cells, '|'-separated.
ARMS = os.environ.get("PR72_ARMS", "").split()
KEYS = [k for k in os.environ.get("PR72_KEYS", "").split("|") if k]


def cpuset(s):
    out = set()
    for part in s.split(","):
        a, _, b = part.partition("-")
        out.update(range(int(a), int(b or a) + 1))
    return out


SETC = cpuset(SET)
W1_N = {"shim_spsc_move": 100_000_000}
W10_N = {"spsc_cell": 100_000_000}
PCS = ["1:1", "2:2", "4:4", "8:8", "1:2", "1:4", "1:8", "2:1", "4:1", "8:1"]
THREADS = [1, 2, 4, 8, 16]


def split(pc): p, c = pc.split(":"); return ["--mode", "split", "--producers", p, "--consumers", c]


# The bounded queue is several times slower than the unbounded ones, and
# W9's count is per producer (per thread in pairwise): WB runs 4 M elements
# in all, divided among the producers (the threads), so a run lasts about
# a second at any count.
WB_TOTAL = 4_000_000
def wbsplit(pc): return split(pc) + ["--n", str(WB_TOTAL // int(pc.split(":")[0]))]
def wbpair(t): return ["--mode", "pairwise", "--threads", str(t), "--n", str(WB_TOTAL // t)]


cells = []  # (key, binary, args, arms)
for v in ["shim_generic", "shim_generic_move", "shim_spsc", "shim_spsc_move"]:
    cells.append((f"W1 {v} E64", "w1_throughput",
                  ["--variant", v, "--elem", "64", "--wait", "spin", "--n", str(W1_N.get(v, 20_000_000))], AB))
for v in ["mpmc_share", "mpmc_cell", "spsc_cell"]:
    cells.append((f"W10 {v} E64", "w10_ablation",
                  ["--variant", v, "--elem", "64", "--wait", "spin", "--n", str(W10_N.get(v, 40_000_000))], AB))
for pc in PCS:
    cells.append((f"W9 split shim_generic {pc}", "w9_mpmc_scaling", ["--variant", "shim_generic"] + split(pc), AB))
for pc in ["1:1", "2:1", "4:1", "8:1"]:
    cells.append((f"W9 split shim_mpsc {pc}", "w9_mpmc_scaling", ["--variant", "shim_mpsc"] + split(pc), AB))
for pc in ["2:1", "8:1"]:
    # Self-contained (benchmarks/queues/mpsc_list_original.hpp): the same
    # source in both trees, a control across them.
    cells.append((f"W9 split orig_mpsc {pc}", "w9_mpmc_scaling", ["--variant", "orig_mpsc"] + split(pc), AB))
for pc in ["1:1", "1:2", "1:4", "1:8"]:
    cells.append((f"W9 split shim_spmc {pc}", "w9_mpmc_scaling", ["--variant", "shim_spmc"] + split(pc), AB))
for t in THREADS:
    cells.append((f"W9 pairwise shim_generic {t}", "w9_mpmc_scaling",
                  ["--variant", "shim_generic", "--mode", "pairwise", "--threads", str(t)], AB))
for v in ["cell_mpmc", "cell_spmc"]:
    for k in [1, 4, 8]:
        cells.append((f"W11 {v} E64 w{k}", "w11_pool",
                      ["--variant", v, "--elem", "64", "--consumers", str(k), "--work", "0", "--rounds", "0",
                       "--seconds", "1"], AB))
cells.append(("WB split bounded_spsc 1:1", "wb_bounded", ["--variant", "bounded_spsc"] + wbsplit("1:1"), AB))
for pc in ["1:1", "2:1", "4:1", "8:1"]:
    cells.append((f"WB split bounded_mpsc {pc}", "wb_bounded", ["--variant", "bounded_mpsc"] + wbsplit(pc), AB))
for pc in ["1:2", "1:4", "1:8"]:
    cells.append((f"WB split bounded_spmc {pc}", "wb_bounded", ["--variant", "bounded_spmc"] + wbsplit(pc), AB))
for v, arms in [("bounded_generic", AB), ("bounded_scq", ["pr"]), ("bounded_wcq", ["pr"])]:
    for pc in PCS:
        cells.append((f"WB split {v} {pc}", "wb_bounded", ["--variant", v] + wbsplit(pc), arms))
    for t in THREADS:
        cells.append((f"WB pairwise {v} {t}", "wb_bounded",
                      ["--variant", v] + wbpair(t), arms))
if ONLY:
    cells = [c for c in cells if any(w in c[0] for w in ONLY)]
if KEYS:
    cells = [c for c in cells if c[0] in KEYS]
if ARMS:
    cells = [(k, b, a, ARMS) for k, b, a, arms in cells if arms == AB]
FIRST = ARMS[0] if ARMS else "main"


def stat():
    d = {}
    for l in open("/proc/stat"):
        if l.startswith("cpu") and l[3].isdigit():
            f = l.split(); d[int(f[0][3:])] = list(map(int, f[1:]))
    return d


def busy(a, b):
    out = []
    for c, v in b.items():
        if c in SETC or c not in a: continue
        dd = [x - y for x, y in zip(v, a[c])]; t = sum(dd)
        out.append((t - dd[3] - dd[4]) / t if t else 0.0)
    return (sum(out) / len(out) if out else 0.0), (max(out) if out else 0.0)


def lock(cmd):
    if MODE == "timed":
        return ["machine-lock", "--lane", "wtc-bench", "--measure", "--minutes", "5", "--why", "wtc pr72 leg",
                "--no-scope", "--"] + cmd
    return ["flock", LOCKF] + cmd if LOCKF and not REP_SCOPE else cmd


import contextlib, fcntl
@contextlib.contextmanager
def held():
    global _held
    if MODE == "timed" or not LOCKF or not REP_SCOPE or _held:
        yield; return
    with open(LOCKF) as f:
        t0 = time.time(); fcntl.flock(f, fcntl.LOCK_EX)
        LOG.write(f"lock held after {time.time() - t0:.0f} s wait\n"); LOG.flush()
        _held = SCOPE == "leg"
        try: yield
        finally:
            _held = False
            fcntl.flock(f, fcntl.LOCK_UN)
_held = False


def freq():
    c = min(SETC)
    try: return open(f"/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_cur_freq").read().strip()
    except OSError: return "unknown"


os.makedirs(OUT, exist_ok=True)
RUNS = open(f"{OUT}/runs.jsonl", "a")
LOG = open(f"{OUT}/log.txt", "a")


def run(tag, rep, key, binary, args, arm):
    cmd = lock(["timeout", "120", "taskset", "-c", SET, f"{BIN}/{arm}/{binary}"] + args + ["--rep", str(rep)])
    s0 = stat(); t0 = time.time()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace")
    wall = time.time() - t0; s1 = stat()
    bm, bx = busy(s0, s1)
    res = None
    for l in p.stdout.splitlines():
        if l.startswith("{"):
            try: res = json.loads(l)
            except ValueError: pass
    r = dict(leg="pr72", mode=MODE, tag=tag, rep=rep, key=key, arm=arm, rc=p.returncode, wall_s=round(wall, 3),
             busy_out_mean=round(bm, 4), busy_out_max=round(bx, 4), freq_khz=freq(),
             time=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), result=res)
    RUNS.write(json.dumps(r) + "\n"); RUNS.flush()
    val = res.get("mops", res.get("tasks_per_s", "?")) if res else "?"
    LOG.write(f"[{r['time']}] {tag} rep={rep} {key} {arm} rc={p.returncode} {val} wall={wall:.2f} busy_out={bm:.3f}\n")
    if p.returncode != 0 or res is None:
        LOG.write(p.stderr[-2000:] + "\n")
    LOG.flush()
    return r


CONTROL = ("W1 shim_generic E64", "w1_throughput", ["--variant", "shim_generic", "--elem", "64", "--wait", "spin",
                                                     "--n", "20000000"])
LOG.write(f"=== pr72 leg {MODE} reps={REPS} cells={len(cells)} set={SET} start {time.strftime('%FT%TZ', time.gmtime())}"
          f" load {open('/proc/loadavg').read().split()[:3]}\n")
if MODE == "timed":
    e = subprocess.run([os.environ["PR72_CHECK_ENV"], str(len(SETC))], capture_output=True, text=True)
    LOG.write(e.stdout + e.stderr); LOG.flush()
    if e.returncode != 0: sys.exit("environment not acceptable")
_leg = held() if SCOPE == "leg" else contextlib.nullcontext()
_leg.__enter__()
with held():
    for k in range(2): run("warmup", -1, *CONTROL, FIRST)
    for k in range(3): run("control-start", 900, *CONTROL, FIRST)
for rep in range(REP0, REP0 + REPS):
    with held():
        for key, binary, args, arms in cells:
            for k in range(len(arms)):
                run(MODE, rep, key, binary, args, arms[(k + rep) % len(arms)])
with held():
    for k in range(3): run("control-end", 901, *CONTROL, FIRST)
_leg.__exit__(None, None, None)
LOG.write(f"=== end {time.strftime('%FT%TZ', time.gmtime())}\n")
