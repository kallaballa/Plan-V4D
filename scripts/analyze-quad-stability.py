#!/usr/bin/env python3
"""Measure how stable the DINOv3 marker outline is across a CSV dump.

The demo's complaint to reproduce: recognition is good, but the green quad
changes shape wildly from frame to frame while staying on the marker. This
script turns that into numbers, so a change in the localiser can be judged
against the previous behaviour instead of by eye.

Reads the CSV the self-test writes (--dump), computes per-frame outline shape
and frame-to-frame shape change, and prints both.

usage: analyze-quad-stability.py DUMP.csv [--clip NAME] [--frame W H]
"""

import argparse
import csv
import math
import sys


def shoelace(pts):
    a = 0.0
    for i in range(len(pts)):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % len(pts)]
        a += x0 * y1 - x1 * y0
    return abs(a) * 0.5


def is_convex(pts):
    """True when all four turns share a sign, i.e. no bowtie."""
    signs = set()
    n = len(pts)
    for i in range(n):
        ax, ay = pts[i]
        bx, by = pts[(i + 1) % n]
        cx, cy = pts[(i + 2) % n]
        cross = (bx - ax) * (cy - by) - (by - ay) * (cx - bx)
        if abs(cross) < 1e-6:
            return False
        signs.add(cross > 0)
    return len(signs) == 1


def quad(pts):
    """Shape descriptors of a 4-point outline, all scale-normalised."""
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    w = max(xs) - min(xs)
    h = max(ys) - min(ys)
    diag = max(math.dist(pts[i], pts[j])
               for i in range(4) for j in range(i + 1, 4))
    sides = [math.dist(pts[i], pts[(i + 1) % 4]) for i in range(4)]
    # Elongation: longest side over shortest. A parallelogram seen head-on is
    # near 1; a sliver or a runaway quad is large.
    elong = max(sides) / max(1e-9, min(sides))
    return {
        "area": shoelace(pts),
        "w": w,
        "h": h,
        "diag": diag,
        "elong": elong,
        "convex": is_convex(pts),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv_path")
    ap.add_argument("--clip", default=None,
                    help="only this clip (default: the only one present)")
    ap.add_argument("--frame", type=int, nargs=2, default=(480, 854),
                    metavar=("W", "H"))
    args = ap.parse_args()

    fw, fh = args.frame
    frame_area = float(fw * fh)

    with open(args.csv_path) as fh_:
        rows = list(csv.DictReader(fh_))

    if not rows:
        print("no rows", file=sys.stderr)
        return 1
    clip = args.clip or rows[0]["clip"]
    rows = [r for r in rows if r["clip"] == clip]

    prev = None
    print(f"== outline shape on {clip} ({fw}x{fh} frame) ==")
    print(f"{'frame':>6} {'area%':>7} {'w':>6} {'h':>6} {'elong':>6} "
          f"{'conv':>5} {'model':>6} {'inl':>5} "
          f"{'dArea%':>7} {'dDiag%':>7} {'dCent%':>7}")

    areas, deltas = [], []
    worst = []
    for r in rows:
        if int(r["verified"]) != 1 or int(r["by"]) != 1:
            prev = None  # a break in the track breaks continuity
            continue
        pts = [(float(r[f"x{i}"]), float(r[f"y{i}"])) for i in range(4)]
        q = quad(pts)
        areas.append(q["area"] / frame_area)

        d_area = d_diag = d_cent = float("nan")
        if prev is not None:
            pf, pq = prev
            d_area = (q["area"] - pq["area"]) / max(1e-9, pq["area"])
            d_diag = (q["diag"] - pq["diag"]) / max(1e-9, pq["diag"])
            pf_c = (sum(p[0] for p in pf) / 4, sum(p[1] for p in pf) / 4)
            q_c = (sum(p[0] for p in pts) / 4, sum(p[1] for p in pts) / 4)
            d_cent = math.dist(pf_c, q_c) / math.hypot(fw, fh)
            deltas.append((abs(d_area), abs(d_diag), abs(d_cent),
                           int(r["frame"])))
            worst.append((max(abs(d_area), abs(d_diag)), int(r["frame"])))
        prev = (pts, q)

        model = "homog" if int(r["model_h"]) else "affine"
        print(f"{int(r['frame']):>6} {100 * q['area'] / frame_area:>7.1f} "
              f"{q['w']:>6.0f} {q['h']:>6.0f} {q['elong']:>6.2f} "
              f"{str(q['convex']):>5} {model:>6} {int(r['dense_inliers']):>5} "
              f"{100 * d_area:>7.1f} {100 * d_diag:>7.1f} {100 * d_cent:>7.1f}")

    if not areas:
        print("no verified dense frames", file=sys.stderr)
        return 1

    def med(v):
        s = sorted(v)
        return s[len(s) // 2]

    def p95(v):
        s = sorted(v)
        return s[min(len(s) - 1, int(0.95 * len(s)))]

    print()
    print(f"localised dense frames        {len(areas)}")
    print(f"area of frame   med {100 * med(areas):.1f}%  "
          f"min {100 * min(areas):.1f}%  max {100 * max(areas):.1f}%  "
          f"spread x{max(areas) / max(1e-9, min(areas)):.1f}")
    if deltas:
        da = [100 * d[0] for d in deltas]
        dd = [100 * d[1] for d in deltas]
        dc = [100 * d[2] for d in deltas]
        print(f"frame-to-frame  n {len(deltas)}")
        print(f"  |dArea|      med {med(da):.1f}%  p95 {p95(da):.1f}%  "
              f"max {max(da):.1f}%")
        print(f"  |dDiagonal|  med {med(dd):.1f}%  p95 {p95(dd):.1f}%  "
              f"max {max(dd):.1f}%")
        print(f"  |dCentre|    med {med(dc):.1f}%  p95 {p95(dc):.1f}%  "
              f"max {max(dc):.1f}%  (as a fraction of the frame diagonal)")
        worst.sort(reverse=True)
        print(f"  worst jumps: " +
              ", ".join(f"frame {f} ({100 * v:.0f}%)" for v, f in worst[:5]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
