#!/usr/bin/env python3
"""The README's result figures, drawn from the evaluation's summaries.

    readme_figures.py [--results benchmarks/results] [--out docs/figures]

For each evaluation leg it reads the summary.csv that summarise.py writes
in the newest results directory of that leg (<date>-eval-w1, -eval-w2, ...)
and draws one SVG into docs/figures/. A leg with no results yet is skipped
with a note, and its figure is left as it was.

The figures are drawn here rather than with plot.py: plot.py's figures are
for reading the data, these are for the README. They stay a few kilobytes,
use the README's palette, and sit on a light panel of their own, so that
they read the same in GitHub's light and dark themes. The variants and
colours are the same in every figure: the program as written grey, a
general-purpose library slate, the compiler's spsc ring blue, its inline
ring orange, the best expert SPSC queue peach.

Rerun after every leg that changes a figure, and commit the figures with
the results they show.
"""
import argparse
import csv
import math
import sys
from pathlib import Path

FONT = "IBM Plex Sans, Segoe UI, Helvetica, Arial, sans-serif"
PANEL, EDGE, INK, MUTED, GRID = "#FBFAF7", "#E4DFD5", "#172033", "#5F6773", "#E2DDD3"
B0, B1, SPSC, INLINE, B2 = "#9AA5B4", "#5B6676", "#2F6FB2", "#C2562A", "#E8A27E"
TBB = "#323B49"  # oneTBB, where it stands beside moodycamel in one panel
W = 900


# --- drawing ---------------------------------------------------------------

def esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Fig:
    def __init__(self, height, title, subtitle, label):
        self.h, self.label = height, label
        self.parts = [f'<rect x="0.5" y="0.5" width="{W - 1}" height="{height - 1}" rx="12" fill="{PANEL}" stroke="{EDGE}"/>',
                      self.text(28, 38, title, 18, INK, "start", 600),
                      self.text(28, 62, subtitle, 14, MUTED, "start")]

    @staticmethod
    def text(x, y, s, size=14, fill=INK, anchor="middle", weight=400):
        w = f' font-weight="{weight}"' if weight != 400 else ""
        return f'<text x="{x:.0f}" y="{y:.0f}" font-size="{size}" fill="{fill}" text-anchor="{anchor}"{w}>{esc(s)}</text>'

    def add(self, *parts):
        self.parts.extend(parts)

    def line(self, x1, y1, x2, y2, stroke=GRID, width=1.5, dash=""):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<line x1="{x1:.0f}" y1="{y1:.0f}" x2="{x2:.0f}" y2="{y2:.0f}" stroke="{stroke}" stroke-width="{width}"{d}/>')

    def rect(self, x, y, w, h, fill, rx=2):
        self.add(f'<rect x="{x:.0f}" y="{y:.0f}" width="{max(w, 1):.0f}" height="{max(h, 1):.0f}" fill="{fill}" rx="{rx}"/>')

    def polyline(self, pts, stroke, width=3, dash=""):
        d = " ".join(f'{"M" if i == 0 else "L"}{x:.0f},{y:.0f}' for i, (x, y) in enumerate(pts))
        da = f' stroke-dasharray="{dash}"' if dash else ""
        self.add(f'<path d="{d}" fill="none" stroke="{stroke}" stroke-width="{width}" stroke-linejoin="round"{da}/>')

    def dot(self, x, y, colour, hollow=False, r=5):
        fill = PANEL if hollow else colour
        self.add(f'<circle cx="{x:.0f}" cy="{y:.0f}" r="{r}" fill="{fill}" stroke="{colour}" stroke-width="2.5"/>')

    def legend(self, items, x, y, gap=22):
        """items: (label, colour, kind) with kind box, line, dash, dot or open."""
        for label, colour, kind in items:
            if kind == "box":
                self.rect(x, y - 11, 14, 14, colour, 3)
                x += 20
            elif kind in ("line", "dash", "dot"):
                self.add(f'<line x1="{x:.0f}" y1="{y - 4:.0f}" x2="{x + 26:.0f}" y2="{y - 4:.0f}" stroke="{colour}" stroke-width="3"'
                         + (' stroke-dasharray="6 4"' if kind == "dash" else "") + "/>")
                if kind == "dot":
                    self.dot(x + 13, y - 4, colour, r=4)
                x += 32
            elif kind == "open":
                self.dot(x + 7, y - 4, colour, hollow=True, r=5)
                x += 20
            self.add(self.text(x, y, label, 13, INK, "start"))
            x += 7.2 * len(label) + gap

    def svg(self):
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{self.h}" viewBox="0 0 {W} {self.h}" '
                f'font-family="{FONT}" role="img" aria-label="{esc(self.label)}"><title>{esc(self.label)}</title>'
                + "".join(self.parts) + "</svg>\n")


def log_axis(lo, hi, a, b):
    """Maps [lo, hi] onto [a, b] on a log scale."""
    return lambda v: a + (b - a) * math.log10(v / lo) / math.log10(hi / lo)


def fmt(v):
    return f"{v:.0f}" if v >= 100 else f"{v:.1f}" if v >= 10 else f"{v:.2f}"


# --- data ------------------------------------------------------------------

class Leg:
    def __init__(self, results, name):
        dirs = sorted(results.glob(f"*-{name}"))
        self.dir = dirs[-1] if dirs else None
        self.rows = []
        if self.dir and (self.dir / "summary.csv").exists():
            self.rows = list(csv.DictReader(open(self.dir / "summary.csv", encoding="utf-8")))

    def __bool__(self):
        return bool(self.rows)

    def get(self, metric="mops", field="median", **want):
        """The one row matching every field given; an empty field matches ''."""
        want.setdefault("pass", "")
        hits = [r for r in self.rows if r["metric"] == metric and all(r.get(k, "") == str(v) for k, v in want.items())]
        if len(hits) != 1:
            raise KeyError(f"{self.dir.name}: {len(hits)} rows for {metric} {want}")
        return float(hits[0][field]) if field != "row" else hits[0]


# --- figures ---------------------------------------------------------------

SERIES = [("shim_generic", "as written (B0)", B0), ("moodycamel_cq_sp", "moodycamel CQ (B1)", B1),
          ("shim_spsc", "compiler: spsc", SPSC), ("shim_spsc_inline", "compiler: inline", INLINE),
          ("rigtorp_inline", "rigtorp, inline (B2)", B2)]


def w1(leg):
    f = Fig(410, "The queue alone: one producer, one consumer",
            "million elements a second, log scale, median of 10 runs; W1, 2 reserved cores", "W1 throughput by element size")
    f.legend([(lab, col, "box") for _, lab, col in SERIES], 28, 92, 16)
    top, bottom, left, right = 112, 350, 64, W - 24
    y = log_axis(1, 300, bottom, top)
    for v in (1, 3, 10, 30, 100, 300):
        f.line(left, y(v), right, y(v))
        f.add(f.text(left - 10, y(v) + 5, v, 13, MUTED, "end"))
    gw, bw = (right - left) / 3, 40
    for gi, elem in enumerate((8, 64, 256)):
        x0 = left + gi * gw + (gw - 5 * bw - 4 * 8) / 2
        for i, (v, _, col) in enumerate(SERIES):
            m = leg.get(workload="W1", elem=elem, wait="spin", variant=v)
            x = x0 + i * (bw + 8)
            f.rect(x, y(m), bw, bottom - y(m), col, 3)
            f.add(f.text(x + bw / 2, y(m) - 6, fmt(m), 13, INLINE if v == "shim_spsc_inline" else INK,
                         weight=600 if v == "shim_spsc_inline" else 400))
        f.add(f.text(left + gi * gw + gw / 2, bottom + 30, f"{elem}-byte elements", 15, INK, weight=600))
    f.line(left, bottom, right, bottom, B0, 2)
    return f


def w2(leg):
    f = Fig(450, "Latency against offered load",
            "p99 from each element's due time, log–log; open points: the runs fell behind; W2, 64-byte elements",
            "W2 p99 latency against offered load")
    f.legend([(lab, col, "line") for _, lab, col in SERIES], 28, 92, 14)
    top, bottom, left, right = 114, 390, 84, W - 28
    x = log_axis(0.1, 10, left, right)
    y = log_axis(300, 1e9, bottom, top)
    for v, s in ((1e3, "1 µs"), (1e4, "10 µs"), (1e5, "100 µs"), (1e6, "1 ms"), (1e7, "10 ms"), (1e8, "100 ms"), (1e9, "1 s")):
        f.line(left, y(v), right, y(v))
        f.add(f.text(left - 10, y(v) + 5, s, 13, MUTED, "end"))
    for v in (0.1, 0.2, 0.5, 1, 2, 5, 10):
        f.add(f.text(x(v), bottom + 22, f"{v:g}", 13, MUTED))
    f.add(f.text((left + right) / 2, bottom + 46, "offered load, million elements a second", 14, MUTED))
    intervals = sorted({int(r["interval_ns"]) for r in leg.rows if r["workload"] == "W2"}, reverse=True)
    for v, _, col in SERIES:
        pts = []
        for iv in intervals:
            p99 = leg.get("p99", workload="W2", mode="open", elem=64, wait="spin", interval_ns=iv, variant=v)
            p50row = leg.get("p50", "row", workload="W2", mode="open", elem=64, wait="spin", interval_ns=iv, variant=v)
            # Behind: a run did not keep the schedule, or the median is a backlog.
            behind = int(p50row["saturated"] or 0) > 0 or float(p50row["median"]) > 1e5
            pts.append((x(1e3 / iv), y(max(p99, 300)), behind))
        f.polyline([(a, b) for a, b, _ in pts], col, 3, "6 4" if v in ("moodycamel_cq_sp", "rigtorp_inline") else "")
        for a, b, behind in pts:
            f.dot(a, b, col, behind)
    f.line(left, bottom, right, bottom, B0, 2)
    return f


def w3(leg):
    rows = [("One source, four builds", None, None),
            ("as written (B0)", ("W3wtc", "generic"), B0), ("tags by hand, by pointer (B3)", ("W3wtc", "hand"), SPSC),
            ("tags by hand, inline (B3)", ("W3wtc", "hand-inline"), B2), ("the compiler", ("W3wtc", "compiler"), INLINE),
            ("A library's queue between the stages", None, None),
            ("oneTBB concurrent_queue (B1)", ("W3", "onetbb_cq_sp"), B1),
            ("moodycamel CQ (B1)", ("W3", "moodycamel_cq_sp"), B1),
            ("rigtorp, inline (B2)", ("W3", "rigtorp_inline"), B2),
            ("moodycamel RWQ, inline (B2)", ("W3", "moodycamel_rw_inline"), B2),
            ("the shim's inline ring", ("W3", "shim_spsc_inline"), INLINE)]
    f = Fig(96 + 34 * len(rows) + 20, "A three-stage pipeline",
            "million records a second, closed loop, median of 10 runs; W3, 64-byte records, 3 reserved cores",
            "W3 pipeline throughput")
    left, scale, yy = 330, 470 / 9.5, 100
    for label, key, col in rows:
        if key is None:
            f.add(f.text(28, yy + 18, label.upper(), 12, MUTED, "start", 600))
        else:
            wl, v = key
            m = leg.get(workload=wl, mode="closed", variant=v, work=4, rate=0) if wl == "W3" else \
                leg.get(workload=wl, mode="closed", variant=v, work=4)
            bold = 600 if v == "compiler" else 400
            f.add(f.text(left - 12, yy + 19, label, 14, INLINE if v == "compiler" else INK, "end", bold))
            f.rect(left, yy + 6, m * scale, 18, col, 3)
            f.add(f.text(left + m * scale + 8, yy + 20, f"{m:.2f}", 14, INK, "start", bold))
        yy += 34
    return f


def grain(leg):
    f = Fig(430, "Where the gain goes",
            "speed-up over the program as written against the time a record takes in the slowest stage; W3 with its work swept",
            "Speed-up against the grain, with the model (t + 1.1 µs) / t")
    f.legend([("the compiler", INLINE, "dot"), ("spsc tags by hand", SPSC, "dot"), ("model: (t + 1.1 µs) / t", INK, "dash")], 28, 92, 18)
    top, bottom, left, right = 112, 370, 64, W - 28
    x = log_axis(0.05, 30000, left, right)
    y = log_axis(0.9, 12, bottom, top)
    # The applications at one thread a stage: run time over elements, which
    # in a pipeline is the slowest stage's time (eval-dedup, eval-ferret).
    for lo, hi, name in ((40, 80, "dedup, 57 µs"), (10000, 18000, "ferret, 13 ms")):
        f.rect(x(lo), top, x(hi) - x(lo), bottom - top, "#EEF1F5", 0)
        f.add(f.text((x(lo) + x(hi)) / 2, y(3.5), name, 13, MUTED))
    for v in (1, 2, 5, 10):
        f.line(left, y(v), right, y(v), B0 if v == 1 else GRID, 2 if v == 1 else 1.5)
        f.add(f.text(left - 10, y(v) + 5, f"{v}×", 13, MUTED, "end"))
    for v in (0.1, 1, 10, 100, 1000, 10000):
        f.add(f.text(x(v), bottom + 22, f"{v:g}", 13, MUTED))
    f.add(f.text((left + right) / 2, bottom + 46, "µs a record, log scale", 14, MUTED))
    # From 4 rounds: at 0 and 4 a record takes the same 0.11 µs, and two
    # points stacked at one x read as a kink in the curve.
    works = sorted({int(r["work"]) for r in leg.rows if r["workload"] == "W3wtc"} - {0})
    t = {w: 1 / leg.get(workload="W3wtc", mode="closed", variant="compiler", work=w) for w in works}
    base = {w: leg.get(workload="W3wtc", mode="closed", variant="generic", work=w) for w in works}
    c = 1.1
    ts = [10 ** (math.log10(0.6) + i * (math.log10(30000) - math.log10(0.6)) / 50) for i in range(51)]
    f.polyline([(x(s), y((s + c) / s)) for s in ts], INK, 1.5, "6 4")
    for v, col in (("hand", SPSC), ("compiler", INLINE)):
        pts = [(x(t[w]), y(leg.get(workload="W3wtc", mode="closed", variant=v, work=w) / base[w])) for w in works]
        f.polyline(pts, col, 3)
        for a, b in pts:
            f.dot(a, b, col)
    return f


def w7(leg):
    f = Fig(462, "Batched pushes: what they buy and what they cost",
            "W7, 64-byte elements, median of 10 runs; left closed loop, right paced at one element a microsecond",
            "W7 throughput and delay against the batch size")
    ks = [("none", None), ("1", 1), ("4", 4), ("16", 16), ("64", 64), ("256", 256), ("1024", 1024)]

    def name(q, kind, k):
        if k is None:
            return f"{q}_plain_e64"
        return f"{q}_{kind}_e64" if k == 64 else f"{q}_{kind}_k{k}_e64"
    # left: the general-purpose queue's throughput
    top, bottom, left, pw = 120, 360, 60, 360
    f.add(f.text(left, 100, "general-purpose queue, M elements/s", 13, MUTED, "start"))
    y = lambda v: bottom - (bottom - top) * v / 2.2
    for v in (0, 0.5, 1, 1.5, 2):
        f.line(left, y(v), left + pw, y(v), B0 if v == 0 else GRID, 2 if v == 0 else 1.5)
        f.add(f.text(left - 8, y(v) + 5, f"{v:g}", 13, MUTED, "end"))
    gw = pw / len(ks)
    for i, (lab, k) in enumerate(ks):
        xc = left + i * gw + gw / 2
        if k is None:
            m = leg.get(workload="W7", mode="closed", variant=name("generic", "", None))
            f.rect(xc - 8, y(m), 16, bottom - y(m), B0)
        else:
            for j, (kind, col) in enumerate((("batch", INLINE), ("hand", B2))):
                m = leg.get(workload="W7", mode="closed", variant=name("generic", kind, k))
                f.rect(xc - 17 + j * 18, y(m), 16, bottom - y(m), col)
        f.add(f.text(xc, bottom + 20, lab, 13, INK))
    f.add(f.text(left + pw / 2, bottom + 42, "batch size K", 13, MUTED))
    f.legend([("as written", B0, "box"), ("compiler", INLINE, "box"), ("by hand", B2, "box")], left, 432, 14)
    # right: median delay at 1 M/s
    l2, pw2 = 520, W - 520 - 28
    f.add(f.text(l2, 100, "median delay at 1 M elements/s, µs", 13, MUTED, "start"))
    y2 = log_axis(0.1, 1000, bottom, top)
    for v in (0.1, 1, 10, 100, 1000):
        f.line(l2, y2(v), l2 + pw2, y2(v), B0 if v == 0.1 else GRID, 2 if v == 0.1 else 1.5)
        f.add(f.text(l2 - 8, y2(v) + 5, f"{v:g}", 13, MUTED, "end"))
    gw2 = pw2 / len(ks)
    for q, col in (("generic", B0), ("spsc", SPSC), ("inline", INLINE)):
        pts = [(l2 + i * gw2 + gw2 / 2, y2(leg.get("p50", workload="W7", mode="open", variant=name(q, "batch", k)) / 1000))
               for i, (_, k) in enumerate(ks)]
        f.polyline(pts, col, 3)
        for a, b in pts:
            f.dot(a, b, col, r=4)
    for i, (lab, _) in enumerate(ks):
        f.add(f.text(l2 + i * gw2 + gw2 / 2, bottom + 20, lab, 13, INK))
    f.add(f.text(l2 + pw2 / 2, bottom + 42, "batch size K", 13, MUTED))
    f.legend([("general-purpose", B0, "line"), ("spsc", SPSC, "line"), ("inline", INLINE, "line")], l2, 432, 14)
    return f


def w8(leg):
    f = Fig(420, "Batched critical sections: when holding the lock pays",
            "speed-up of the batched loop (K = 64) over the loop as written, log scale; W8, one mutex, 8 reserved cores",
            "W8 speed-up against the work between sections")
    f.legend([("2 threads", B2, "dot"), ("4 threads", INLINE, "dot"), ("8 threads", INK, "dot")], 28, 92, 18)
    top, bottom, left, right = 112, 350, 64, W - 28
    works = [0, 20, 50, 100, 200, 500]
    x = lambda i: left + 30 + i * (right - left - 60) / (len(works) - 1)
    y = log_axis(0.3, 15, bottom, top)
    f.rect(left, y(1), right - left, bottom - y(1), "#F6E3D8", 0)
    f.add(f.text(left + 12, y(0.38), "below 1×: batching loses", 13, INLINE, "start"))
    for v in (0.3, 0.5, 1, 2, 5, 10):
        f.line(left, y(v), right, y(v), B0 if v == 1 else GRID, 2 if v == 1 else 1.5)
        f.add(f.text(left - 10, y(v) + 5, f"{v:g}×", 13, MUTED, "end"))
    for i, w in enumerate(works):
        f.add(f.text(x(i), bottom + 22, w, 13, MUTED))
    f.add(f.text((left + right) / 2, bottom + 46, "work between sections, ns", 14, MUTED))
    for t, col in (("2", B2), ("4", INLINE), ("8", INK)):
        pts = [(x(i), y(leg.get(workload="W8", variant="batched", threads=t, work=f"{w}ns", k=64)
                       / leg.get(workload="W8", variant="plain", threads=t, work=f"{w}ns", k=1))) for i, w in enumerate(works)]
        f.polyline(pts, col, 3)
        for a, b in pts:
            f.dot(a, b, col)
    return f


def w9(leg):
    f = Fig(430, "Many threads: what the compiler substitutes, and what it leaves",
            "million elements a second, median of 10 runs; W9, producers and consumers on their own cores, 16 reserved cores",
            "W9 throughput of the general-purpose queues and of the mpsc and spmc substitutes")
    m = lambda v, p, c: leg.get(workload="W9", mode="split", producers=p, consumers=c, variant=v)
    top, bottom = 120, 350
    # left: many on both sides, the general queue against the libraries
    left, pw = 60, 340
    f.add(f.text(left, 100, "many producers, many consumers (P = C)", 13, MUTED, "start"))
    ps = [1, 2, 4, 8]
    both = ("shim_generic", "moodycamel_cq_sp", "onetbb_cq_sp")
    hi = math.ceil(max(m(v, p, p) for v in both for p in ps))
    y = lambda v: bottom - (bottom - top) * v / (hi + 0.5)
    for v in range(hi + 1):
        f.line(left, y(v), left + pw, y(v), B0 if v == 0 else GRID, 2 if v == 0 else 1.5)
        f.add(f.text(left - 8, y(v) + 5, v, 13, MUTED, "end"))
    x = lambda i: left + 24 + i * (pw - 48) / (len(ps) - 1)
    for v, col, dash in (("shim_generic", B0, ""), ("moodycamel_cq_sp", B1, ""), ("onetbb_cq_sp", B1, "6 4")):
        pts = [(x(i), y(m(v, p, p))) for i, p in enumerate(ps)]
        f.polyline(pts, col, 3, dash)
        for a, b in pts:
            f.dot(a, b, col, r=4)
    for i, p in enumerate(ps):
        f.add(f.text(x(i), bottom + 20, f"{p}:{p}", 13, INK))
    f.legend([("as written (B0)", B0, "line"), ("moodycamel CQ", B1, "line"), ("oneTBB", B1, "dash")], left, 400, 12)
    # right: one side single, the general queue, the compiler's substitute, the best library
    l2, pw2 = 500, W - 500 - 28
    f.add(f.text(l2, 100, "one side single: the compiler's mpsc and spmc", 13, MUTED, "start"))
    shapes = [("2", "1"), ("4", "1"), ("8", "1"), ("1", "2"), ("1", "4"), ("1", "8")]
    bars = lambda c: (("shim_generic", B0), ("shim_mpsc" if c == "1" else "shim_spmc", SPSC),
                      ("moodycamel_cq_sp", B1), ("onetbb_cq_sp", TBB))
    hi2 = math.ceil(max(m(v, p, c) for p, c in shapes for v, _ in bars(c)))
    y2 = lambda v: bottom - (bottom - top) * v / (hi2 + 0.5)
    for v in range(hi2 + 1):
        f.line(l2, y2(v), l2 + pw2, y2(v), B0 if v == 0 else GRID, 2 if v == 0 else 1.5)
        f.add(f.text(l2 - 8, y2(v) + 5, v, 13, MUTED, "end"))
    gw = pw2 / len(shapes)
    for i, (p, c) in enumerate(shapes):
        xc = l2 + i * gw + gw / 2
        for j, (v, col) in enumerate(bars(c)):
            val = m(v, p, c)
            f.rect(xc - 27 + j * 14, y2(val), 12, bottom - y2(val), col)
        f.add(f.text(xc, bottom + 20, f"{p}:{c}", 13, INK))
    f.legend([("as written", B0, "box"), ("compiler", SPSC, "box"), ("moodycamel CQ", B1, "box"),
              ("oneTBB", TBB, "box")], l2, 400, 12)
    return f


FIGURES = [("eval-w1", "w1", w1), ("eval-w2", "w2", w2), ("eval-w3", "w3", w3),
           ("eval-w3grain", "grain", grain), ("eval-w7", "w7", w7), ("eval-w8", "w8", w8),
           ("eval-w9", "w9", w9)]


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    root = Path(__file__).resolve().parents[2]
    p.add_argument("--results", type=Path, default=root / "benchmarks" / "results")
    p.add_argument("--out", type=Path, default=root / "docs" / "figures")
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    for legname, name, draw in FIGURES:
        leg = Leg(a.results, legname)
        if not leg:
            print(f"{name}: no summary.csv under *-{legname}; left as it was", file=sys.stderr)
            continue
        (a.out / f"{name}.svg").write_text(draw(leg).svg(), encoding="utf-8")
        print(f"{name}.svg from {leg.dir.name}")


if __name__ == "__main__":
    main()
