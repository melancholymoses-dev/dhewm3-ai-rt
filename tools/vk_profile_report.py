#!/usr/bin/env python3
"""
Analyse an r_vkRTProfileCSV capture from the dhewm3-rt Vulkan profiler.

Opening move on any capture, before looking at individual frames.  Answers, in order:
is this capture usable, which side is setting the pace, which passes are expensive, and
which of them are unstable enough to be worth tuning.

  1. CONDITIONS  - what the capture contains and what it therefore cannot speak to
                   (lights.serial == 0 says nothing about serial shadow dispatch), plus
                   GPU-bound vs CPU-bound from the backend with blocking waits removed.
  2. CONSISTENCY - frame.backend against the sum of its accounted parts.  A gap means
                   a span is missing, not that the engine has mystery time.
  3. SWINGS      - passes whose p95 is over 0.5 ms above their p50, labelled with the
                   cause where it is testable: the measurement itself, frame pacing, or
                   scene content (the last being the only tuning target).
  4. TIMES       - ms per column, ranked by p50 so the expensive passes come first.
  5. COUNTERS    - draws, bytes, allocations, lights, RSS/VRAM.  Monitor, don't rank.
  6. WORST       - --top N frames by a chosen column, all non-zero fields.

With --baseline, diffs two captures instead, which is what the arc 8 stages need for
their before/after numbers.

Usage:
    python tools/vk_profile_report.py a0.csv
    python tools/vk_profile_report.py a0.csv --group gpu.
    python tools/vk_profile_report.py a0.csv --top 3 --sort-by cpu.TLAS
    python tools/vk_profile_report.py after.csv --baseline before.csv

This file is a new addition with dhewm3-rt.  It was created with the aid of GenAI,
and may reference the existing Dhewm3 OpenGL and vkDoom3 Vulkan updates of the Doom 3 GPL Source
Code.

It is distributed under the same modified GNU General Public License Version 3 of the original Doom 3 GPL Source
Code release.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from collections import Counter
from typing import Dict, List, Optional, Sequence

# Terms that should add up to frame.backend.  Anything outside this list that costs
# time lands in frame.other, which is the signal to add a span for it.
ACCOUNTED = [
    "frame.fenceWait",
    "frame.acquire",
    "frame.drain",
    "frame.upload",
    "frame.submit",
    "frame.present",
    "frame.other",
    "frame.profilePrint",
    "wait.readback.ms",
    "wait.serialize.ms",
]
# Per-phase CPU also counts toward frame.backend; cpu.total and cpu.raster are its
# two halves.  Nested sub-spans (cpu.TLAS.BLAS) are inside a parent and must not be
# added again.
ACCOUNTED_PHASE_CPU = ["cpu.total", "cpu.raster"]

# Columns that are counts or byte totals rather than milliseconds.  They are worth
# monitoring but must not be mixed into a table you scan for expensive passes.
NON_TIME = ("KB", "MB", ".draws", ".allocs", ".frees", ".blocks", "staticBlocks", ".tris",
            "lights.", ".flushes", ".n", ".updates", ".rebuilds", ".dynInst", ".overflows",
            ".imageLoads", ".pipelines")


def percentile(sorted_vals: Sequence[float], q: float) -> float:
    if not sorted_vals:
        return 0.0
    idx = int(q * (len(sorted_vals) - 1) + 0.5)
    return sorted_vals[min(idx, len(sorted_vals) - 1)]


class Capture:
    def __init__(self, path: str):
        self.path = path
        self.cols: Dict[str, List[float]] = {}
        self.frames: List[int] = []
        with open(path, newline="") as fh:
            reader = csv.DictReader(fh)
            if not reader.fieldnames:
                raise SystemExit(f"{path}: empty or not a CSV")
            names = [n for n in reader.fieldnames if n]
            for n in names:
                self.cols[n] = []
            for row in reader:
                for n in names:
                    try:
                        self.cols[n].append(float(row[n] or 0.0))
                    except (TypeError, ValueError):
                        self.cols[n].append(0.0)
        self.frames = [int(v) for v in self.cols.get("frame", [])]
        self.n = len(self.frames)
        if self.n == 0:
            raise SystemExit(f"{path}: no data rows")
        self._sorted: Dict[str, List[float]] = {}

    def has(self, name: str) -> bool:
        return name in self.cols

    def col(self, name: str) -> List[float]:
        return self.cols.get(name, [0.0] * self.n)

    def srt(self, name: str) -> List[float]:
        if name not in self._sorted:
            self._sorted[name] = sorted(self.col(name))
        return self._sorted[name]

    def stat(self, name: str, q: float) -> float:
        return percentile(self.srt(name), q)

    def mean(self, name: str) -> float:
        c = self.col(name)
        return sum(c) / len(c) if c else 0.0

    def live_columns(self) -> List[str]:
        out = []
        for n in self.cols:
            if n == "frame":
                continue
            if max(self.col(n)) != 0.0:
                out.append(n)
        return out


# ---------------------------------------------------------------------------
# 1. Conditions
# ---------------------------------------------------------------------------


def report_conditions(cap: Capture, periodic: Dict[str, int]) -> None:
    print("=" * 78)
    print(f"CONDITIONS  {cap.path}  ({cap.n} frames, {cap.frames[0]}..{cap.frames[-1]})")
    print("=" * 78)

    def line(label: str, text: str) -> None:
        print(f"  {label:<26} {text}")

    if cap.has("lights.serial"):
        serial_max = max(cap.col("lights.serial"))
        line(
            "shadow lights",
            "view %d  casting %d  batched %d  serial %d (p50 / max %d)"
            % (
                cap.stat("lights.view", 0.5),
                cap.stat("lights.shadow", 0.5),
                cap.stat("lights.batched", 0.5),
                cap.stat("lights.serial", 0.5),
                serial_max,
            ),
        )
        # A zero caster count alongside a real gpu.Shadows cost is a broken counter, not
        # a scene with no shadows; that combination hid the GUI-view reset bug for three
        # captures.  Say which of the two it is rather than blaming the scene.
        if max(cap.col("lights.shadow")) == 0 and cap.stat("gpu.Shadows", 0.5) > 0.2:
            line("", "!! lights.shadow is 0 in every frame but gpu.Shadows p50 is %.2f ms."
                 % cap.stat("gpu.Shadows", 0.5))
            line("", "   The counter is wrong, not the scene. Nothing here speaks to")
            line("", "   shadow dispatch until it is fixed and recaptured.")
        elif serial_max == 0:
            line("", "!! lights.serial is 0 in every frame: this capture CANNOT")
            line("", "   speak to serial shadow dispatch (cleanup doc Phase 6a).")
            line("", "   Serial time is billed to Interactions, so a low")
            line("", "   gpu.Interactions here means 'never happened', not 'cheap'.")
    else:
        line("shadow lights", "not in this CSV (pre-lights.* capture)")

    if cap.has("mem.rssMB"):
        # First rows can be zero (the gauge is sampled at frame open and the first
        # pushed frame predates it), so drift is read from non-zero samples only.
        rss = [v for v in cap.col("mem.rssMB") if v > 0.0]
        if rss:
            k = max(1, len(rss) // 20)
            first, last = sum(rss[:k]) / k, sum(rss[-k:]) / k
            line(
                "RSS MB",
                "%.0f -> %.0f (drift %+.0f over %d frames), peak %.0f"
                % (first, last, last - first, len(rss), max(rss)),
            )
        if cap.has("mem.vramUsedMB"):
            line(
                "VRAM MB",
                "%.0f used of %.0f budget (%.0f%%)"
                % (
                    cap.stat("mem.vramUsedMB", 0.5),
                    cap.stat("mem.vramBudgetMB", 0.5),
                    100.0 * cap.stat("mem.vramUsedMB", 0.5) / max(1.0, cap.stat("mem.vramBudgetMB", 0.5)),
                ),
            )
            if cap.stat("mem.vramUsedMB", 0.5) < 0.8 * cap.stat("mem.vramBudgetMB", 0.5):
                line("", "no VRAM pressure: audit stages A4 and F12 have no evidence here")

    if cap.has("midframe.imageLoads"):
        loads = sum(cap.col("midframe.imageLoads"))
        line("mid-frame image loads", "%d total" % loads)
        if loads == 0:
            line("", "F3 (lazy load during recording) did not occur in this capture")

    waits = [n for n in cap.cols if n.startswith("wait.") and n.endswith(".ms") and max(cap.col(n)) > 0.0]
    if waits:
        line("blocking waits seen", ", ".join("%s max %.2f" % (w.split(".")[1], max(cap.col(w))) for w in waits))
    else:
        line("blocking waits seen", "none (every wait tag is zero)")

    gt, gr = cap.stat("gpu.total", 0.5), cap.stat("gpu.raster", 0.5)
    if gt or gr:
        line("GPU split p50", "RT %.3f  raster %.3f  (raster is %.0f%% of GPU)" % (gt, gr, 100.0 * gr / max(1e-9, gt + gr)))

    # A backend that includes a blocking wait is not CPU work.  Subtract every wait tag
    # and the profiler's own print to get what the CPU actually does per frame, then say
    # which side is setting the pace — quoting frame.backend without this reads a GPU-bound
    # capture as a CPU problem.
    if cap.has("frame.backend") and (gt or gr):
        waitcols = [n for n in cap.cols if n.startswith("wait.") and n.endswith(".ms")]
        waitcols += [n for n in ("frame.profilePrint", "frame.fenceWait") if cap.has(n)]
        busy = sorted(
            max(0.0, cap.col("frame.backend")[i] - sum(cap.col(w)[i] for w in waitcols))
            for i in range(cap.n)
        )
        cpu_busy, gpu_p50 = percentile(busy, 0.5), gt + gr
        line("pacing p50", "GPU %.2f ms vs CPU busy %.2f ms (backend %.2f, waits removed)"
             % (gpu_p50, cpu_busy, cap.stat("frame.backend", 0.5)))
        if gpu_p50 > 1.5 * cpu_busy:
            line("", "GPU-BOUND: CPU-side wins here only shorten a wait, not the frame.")
            line("", "   Expect the spare time to show up in whichever wait tag blocks.")
        elif cpu_busy > 1.5 * gpu_p50:
            line("", "CPU-BOUND: the backend is the frame. CPU-side work pays off.")

    if periodic:
        line("MEASUREMENT ARTIFACT",
             ", ".join("%s (every %d frames)" % (n, g) for n, g in periodic.items()))
        line("", "spikes at a fixed interval are the profiler, not the game; ignore")
        line("", "their tails. Capture with r_vkRTProfileCSV set to avoid this.")
    print()


# ---------------------------------------------------------------------------
# 2. Artifacts: periodic spikes at the summary-window interval
# ---------------------------------------------------------------------------


def detect_periodic(cap: Capture, min_share: float = 0.6) -> Dict[str, int]:
    """{column: gap} for columns whose spikes land at a fixed frame interval.

    A spike that is exactly N frames apart every time is the measurement, not the game.
    Only ever found frame.profilePrint, which the engine no longer prints while a CSV is
    open, so this is a silent guard against the next such mistake rather than a report.
    """
    periodic: Dict[str, int] = {}
    for name in cap.live_columns():
        vals = cap.col(name)
        p50, mx = cap.stat(name, 0.5), max(vals)
        # Only worth reporting when the periodic spike is big enough to distort a
        # reading.  The window-close CSV flush is periodic and costs ~0.6 ms, which is
        # true and useless; the print this was written to catch cost 15-20 ms.
        if mx < 3.0 or mx < 4.0 * max(p50, 1e-6):
            continue
        thresh = max(p50 * 4.0, mx * 0.4)
        hits = [cap.frames[i] for i, v in enumerate(vals) if v > thresh]
        if len(hits) < 4:
            continue
        gaps = Counter(hits[i + 1] - hits[i] for i in range(len(hits) - 1))
        gap, count = gaps.most_common(1)[0]
        if count / (len(hits) - 1) >= min_share and gap > 1:
            periodic[name] = gap
    return periodic


def gpu_cost(cap: Capture, i: int) -> float:
    return cap.col("gpu.total")[i] + cap.col("gpu.raster")[i]


def backpressure_columns(cap: Capture, candidates: Sequence[str]) -> Dict[str, float]:
    """Columns that grow with the PREVIOUS frame's GPU cost.

    A blocking wait that tracks last frame's GPU time is the CPU parking until the GPU
    catches up — frame pacing, not a cost of whatever the wait is named after.  Tested by
    bucketing frames into GPU-cost quintiles: monotonic means plus a big top/bottom ratio.
    Returns {column: ratio}.
    """
    if cap.n < 50 or not (cap.has("gpu.total") or cap.has("gpu.raster")):
        return {}
    order = sorted(range(1, cap.n), key=lambda i: gpu_cost(cap, i - 1))
    q = len(order) // 5
    if q < 5:
        return {}
    out: Dict[str, float] = {}
    for name in candidates:
        if not cap.has(name):
            continue
        vals = cap.col(name)
        means = [sum(vals[i] for i in order[b * q:(b + 1) * q]) / q for b in range(5)]
        if means[0] < 1e-6:
            continue
        if all(means[b + 1] >= means[b] for b in range(4)) and means[4] / means[0] >= 3.0:
            out[name] = means[4] / means[0]
    return out


# ---------------------------------------------------------------------------
# 3. Consistency: does frame.backend close?
# ---------------------------------------------------------------------------


def report_shadow_dispatch(cap: Capture) -> None:
    """Pass cost bucketed by how many shadow lights overflowed the batch.

    Serial shadow dispatch is billed to gpu.Interactions, so the two cannot be told apart
    from a single p50.  Varying lights.serial within one capture separates them: if
    Interactions rises with it while Shadows falls, work is moving from the batched
    dispatch into the serial path.
    """
    if not cap.has("lights.serial") or max(cap.col("lights.serial")) == 0:
        return
    serial = [int(v) for v in cap.col("lights.serial")]
    if len(set(serial)) < 4:
        return

    print("=" * 78)
    print("SHADOW DISPATCH  (cost vs shadow lights that overflowed the batch)")
    print("=" * 78)
    batched_cap = cap.stat("lights.batched", 0.95)
    over = sum(1 for s in serial if s > 0)
    print("  batch cap appears to be %d layers; %d of %d frames (%.0f%%) overflow it"
          % (batched_cap, over, cap.n, 100.0 * over / cap.n))
    print("  %8s %6s %9s %9s %9s %9s" % ("serial", "n", "Interact", "Shadows", "Int+Shad", "casters"))

    buckets: Dict[int, List[int]] = {}
    for i, s in enumerate(serial):
        buckets.setdefault(s, []).append(i)

    def med(idxs: List[int], name: str) -> float:
        v = sorted(cap.col(name)[i] for i in idxs)
        return percentile(v, 0.5)

    shown = [s for s in sorted(buckets) if len(buckets[s]) >= 20]
    for s in shown:
        idxs = buckets[s]
        it, sh = med(idxs, "gpu.Interactions"), med(idxs, "gpu.Shadows")
        print("  %8d %6d %9.2f %9.2f %9.2f %9.0f"
              % (s, len(idxs), it, sh, it + sh, med(idxs, "lights.shadow")))

    if len(shown) >= 2:
        lo, hi = buckets[shown[0]], buckets[shown[-1]]
        d_int = med(hi, "gpu.Interactions") - med(lo, "gpu.Interactions")
        d_sh = med(hi, "gpu.Shadows") - med(lo, "gpu.Shadows")
        span = shown[-1] - shown[0]
        if d_int > 0.5 and d_sh < 0.0:
            print("  CONFIRMED: Interactions +%.2f ms and Shadows %.2f ms across %d serial"
                  % (d_int, d_sh, span))
            print("             lights = %.2f ms per serial light, billed to Interactions."
                  % (d_int / max(1, span)))
        else:
            print("  Interactions %+.2f ms, Shadows %+.2f ms across %d serial lights:"
                  % (d_int, d_sh, span))
            print("             no clean handoff; the serial path is not the cost here.")
    print()


def report_consistency(cap: Capture) -> None:
    print("=" * 78)
    print("CONSISTENCY  (frame.backend vs the sum of its parts, per frame)")
    print("=" * 78)
    if not cap.has("frame.backend"):
        print("  frame.backend missing; skipped")
        print()
        return

    # frame.other is DEFINED as backend minus everything else, so including it in the
    # sum makes this check tautological (it returns a residual of exactly 0 and proves
    # nothing).  Sum the real terms only, then compare the residual against frame.other:
    # agreement validates the engine-side arithmetic, and the residual itself is the
    # honest "how much of the backend has no span" number.
    present = [c for c in ACCOUNTED if cap.has(c) and c != "frame.other"]
    phase = [c for c in ACCOUNTED_PHASE_CPU if cap.has(c)]
    missing = [c for c in ACCOUNTED if not cap.has(c)]

    resid, mismatch = [], []
    for i in range(cap.n):
        acc = sum(cap.col(c)[i] for c in present) + sum(cap.col(c)[i] for c in phase)
        r = cap.col("frame.backend")[i] - acc
        resid.append(r)
        mismatch.append(abs(r - cap.col("frame.other")[i]))

    print("  terms summed: %s" % ", ".join(present + phase))
    if missing:
        print("  terms absent from this CSV: %s" % ", ".join(missing))

    mm = sorted(mismatch)
    print(
        "  |residual - frame.other|: p50 %.4f  p95 %.4f  max %.4f ms"
        % (percentile(mm, 0.5), percentile(mm, 0.95), mm[-1])
    )
    if percentile(mm, 0.95) < 0.05:
        print("  OK: frame.other matches the measured residual, so the spans are complete")
    else:
        worst = max(range(cap.n), key=lambda i: mismatch[i])
        print(
            "  !! frame.other disagrees with the residual by %.2f ms on frame %d —"
            % (mismatch[worst], cap.frames[worst])
        )
        print("     a term is counted on one side only (check ACCOUNTED in this script)")

    rs = sorted(resid)
    share = 100.0 * cap.mean("frame.other") / max(1e-9, cap.mean("frame.backend"))
    print(
        "  unattributed backend time: p50 %.3f  p95 %.3f  max %.3f ms  (%.0f%% of mean backend)"
        % (percentile(rs, 0.5), percentile(rs, 0.95), rs[-1], share)
    )
    if share > 25.0:
        print("     >25%: worth adding a span rather than guessing. Check ARTIFACTS first.")
    print()


# ---------------------------------------------------------------------------
# 4. Bimodality
# ---------------------------------------------------------------------------


def report_swings(cap: Capture, periodic: Dict[str, int], ratio: float = 2.5) -> None:
    print("=" * 78)
    print("SWINGS  (p95 - p50 over 0.5 ms: passes whose cost is not stable)")
    print("=" * 78)
    # A 10x ratio on a 0.001 ms pass is noise.  The question is which passes move by an
    # amount worth tuning, so the gate is absolute milliseconds, with the ratio only as
    # a tiebreak for ordering.
    rows = []
    for name in sorted(cap.live_columns()):
        if not is_time(name):
            continue
        p50, p95 = cap.stat(name, 0.5), cap.stat(name, 0.95)
        if p95 - p50 >= 0.5 and (p50 < 1e-6 or p95 / p50 >= ratio):
            rows.append((p95 - p50, name, p50, p95, max(cap.col(name))))
    if not rows:
        print("  none: every pass costs about the same every frame")
        print()
        return

    # A swing with a known cause is not a tuning target.  Two causes are testable:
    # periodicity at the window interval (the profiler's own print) and tracking the
    # previous frame's GPU cost (the CPU waiting on the GPU, wherever that wait sits).
    waits = [n for _, n, _, _, _ in rows if n.startswith("wait.") or n in ("frame.upload", "frame.fenceWait", "frame.backend")]
    bp = backpressure_columns(cap, waits)
    print("  %-24s %9s %9s %9s   %s" % ("metric", "p50", "p95", "max", "swing / cause"))
    for d, name, p50, p95, mx in sorted(rows, reverse=True)[:15]:
        if name in periodic:
            why = "TOOL: periodic every %d frames" % periodic[name]
        elif name in bp:
            why = "PACING: tracks prev-frame GPU (%.0fx)" % bp[name]
        else:
            why = "scene-dependent"
        print("  %-24s %9.3f %9.3f %9.3f   +%.2f ms  %s" % (name, p50, p95, mx, d, why))
    print()


# ---------------------------------------------------------------------------
# 5. Stats table
# ---------------------------------------------------------------------------


def is_time(name: str) -> bool:
    """Milliseconds, as opposed to a count or a byte total."""
    return not any(t in name for t in NON_TIME)


def report_stats(cap: Capture, group: Optional[str], floor: float = 0.05) -> None:
    hdr = "  %-24s %9s %9s %9s %9s %9s %9s"
    row = "  %-24s %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f"
    cols = [n for n in cap.live_columns() if not group or group in n]

    # Alphabetical, which groups cpu./frame./gpu./wait. into blocks and keeps the row
    # order identical run to run so two reports can be read side by side.  Cheap columns
    # are rolled up into one line instead, so the table is still scannable.
    times = sorted(n for n in cols if is_time(n))
    big = [n for n in times if cap.stat(n, 0.5) >= floor or max(cap.col(n)) >= 1.0]
    small = [n for n in times if n not in big]

    print("=" * 78)
    print("TIMES ms  (alphabetical%s)" % (f", filtered on '{group}'" if group else ""))
    print("=" * 78)
    print(hdr % ("metric", "mean", "p5", "p50", "p95", "p99", "max"))
    for n in big:
        print(row % (n, cap.mean(n), cap.stat(n, 0.05), cap.stat(n, 0.5),
                     cap.stat(n, 0.95), cap.stat(n, 0.99), max(cap.col(n))))
    if small:
        print("  %d more under %.2f ms p50 with no spike: %s"
              % (len(small), floor, ", ".join(sorted(small))))
    print()

    counters = sorted(n for n in cols if not is_time(n))
    if counters:
        print("=" * 78)
        print("COUNTERS  (per frame unless the name says otherwise)")
        print("=" * 78)
        print(hdr % ("metric", "mean", "p5", "p50", "p95", "p99", "max"))
        for n in counters:
            print(row % (n, cap.mean(n), cap.stat(n, 0.05), cap.stat(n, 0.5),
                         cap.stat(n, 0.95), cap.stat(n, 0.99), max(cap.col(n))))
        print()


# ---------------------------------------------------------------------------
# 6. Worst frames
# ---------------------------------------------------------------------------


def report_worst(cap: Capture, sort_by: str, top: int, floor: float) -> None:
    if not cap.has(sort_by):
        print(f"WORST: column '{sort_by}' not in this CSV; skipped\n")
        return
    print("=" * 78)
    print(f"WORST {top} FRAMES by {sort_by}")
    print("=" * 78)
    order = sorted(range(cap.n), key=lambda i: cap.col(sort_by)[i], reverse=True)[:top]
    for i in order:
        print("  --- frame %d (%s = %.3f)" % (cap.frames[i], sort_by, cap.col(sort_by)[i]))
        for name in cap.live_columns():
            v = cap.col(name)[i]
            if v > floor:
                print("      %-24s %.4f" % (name, v))
    print()


# ---------------------------------------------------------------------------
# Baseline diff
# ---------------------------------------------------------------------------


def report_diff(cur: Capture, base: Capture, min_pct: float) -> None:
    print("=" * 78)
    print("DIFF  p50, %s (after) vs %s (before)" % (cur.path, base.path))
    print("=" * 78)
    print("  %-24s %10s %10s %10s %8s" % ("metric", "before", "after", "delta", "pct"))
    names = sorted(set(cur.live_columns()) | set(base.live_columns()))
    rows = []
    for name in names:
        b, a = base.stat(name, 0.5), cur.stat(name, 0.5)
        if abs(b) < 1e-6 and abs(a) < 1e-6:
            continue
        pct = 100.0 * (a - b) / b if abs(b) > 1e-6 else math.inf
        if abs(pct) < min_pct and abs(a - b) < 0.05:
            continue
        rows.append((abs(a - b), name, b, a, pct))
    for _, name, b, a, pct in sorted(rows, reverse=True):
        print("  %-24s %10.3f %10.3f %+10.3f %+7.1f%%" % (name, b, a, a - b, pct))
    if not rows:
        print("  no column moved by more than %.0f%% or 0.05" % min_pct)
    print()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="capture written by r_vkRTProfileCSV")
    ap.add_argument("--baseline", help="second capture; print a p50 diff instead of the full report")
    ap.add_argument("--top", type=int, default=0, help="worst-frame dumps (default 0, 0 to skip)")
    ap.add_argument("--sort-by", default="frame.backend", help="column the worst-frame dump ranks on")
    ap.add_argument("--floor", type=float, default=0.02, help="hide fields below this in worst-frame dumps")
    ap.add_argument("--group", help="substring filter for the stats table, e.g. gpu. or wait.")
    ap.add_argument("--min-pct", type=float, default=5.0, help="diff mode: hide moves under this percent")
    args = ap.parse_args()

    cap = Capture(args.csv)

    if args.baseline:
        report_diff(cap, Capture(args.baseline), args.min_pct)
        return 0

    periodic = detect_periodic(cap)
    report_conditions(cap, periodic)
    report_shadow_dispatch(cap)
    report_consistency(cap)
    report_swings(cap, periodic)
    report_stats(cap, args.group)
    if args.top > 0:
        report_worst(cap, args.sort_by, args.top, args.floor)
    return 0


if __name__ == "__main__":
    sys.exit(main())
