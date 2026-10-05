#!/usr/bin/env python3
"""Read a correspondence field dumped by the self-test and say what it is.

`--dump-similarity N:FILE` in the self-test writes, for one frame, every marker
patch's argmax frame patch and the neighbourhood scores there. The question this
answers is the one the funnel counters cannot: is the argmax map a *coherent* map
of the marker onto the frame, or is it a smooth drift across a similarity field
that is nearly flat everywhere?

Those two produce the same `votes_tried` and `vote_best_inliers`, they look the
same in the demo, and they call for opposite responses -- so the field gets
looked at directly rather than inferred from counts.

Prints the 14x14 argmax map, then three verdicts:

  distinct    how many frame patches the marker patches land on. A real marker at
              a known scale lands on a region of the frame of about the marker's
              own area; a drift lands on a handful, or on nearly all of them.
  affine fit  least squares over all correspondences, reported in *frame patch
              cells*, which is the unit the vote's own tolerance is measured in.
              A real marker leaves sub-cell residuals. A drift leaves a smooth,
              slowly varying error that no transform describes.
  ranks       Spearman correlation of the context score against rank. If the best
              match is a real correspondence the score should not be merely the
              largest of 196 near-identical numbers.

usage: analyze-correspondences.py FIELD.csv [--map]
"""

import argparse
import csv
import math
import sys


def read_field(path):
    """Returns (meta, rows). Comment lines are `# key value key value`, except the
    grid line, which is `# marker_grid 14x14`."""
    meta, rows = {}, []
    with open(path) as fh:
        for line in fh:
            if line.startswith("#"):
                tokens = line[1:].split()
                for i in range(0, len(tokens) - 1, 2):
                    meta[tokens[i]] = tokens[i + 1]
                continue
            rows.append(line)
    return meta, list(csv.DictReader(rows))


def solve_affine(src, dst):
    """Least squares for the 2x3 affine taking src onto dst; returns a callable."""
    # Normal equations for each output coordinate, shared design matrix.
    ata = [[0.0] * 3 for _ in range(3)]
    atb = [[0.0] * 2 for _ in range(3)]
    for (x, y), (u, v) in zip(src, dst):
        row = [x, y, 1.0]
        for i in range(3):
            for j in range(3):
                ata[i][j] += row[i] * row[j]
            atb[i][0] += row[i] * u
            atb[i][1] += row[i] * v
    return ata, atb


def solve3(m):
    """Gaussian elimination with partial pivoting; m is n x (n+1)."""
    n = len(m)
    a = [row[:] for row in m]
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(a[r][col]))
        if abs(a[piv][col]) < 1e-12:
            return None
        a[col], a[piv] = a[piv], a[col]
        for r in range(n):
            if r == col:
                continue
            f = a[r][col] / a[col][col]
            for c in range(col, n + 1):
                a[r][c] -= f * a[col][c]
    return [a[i][n] / a[i][i] for i in range(n)]


def affine_coeffs(src, dst):
    ata, atb = solve_affine(src, dst)
    m0 = [ata[i] + [atb[i][0]] for i in range(3)]
    m1 = [ata[i] + [atb[i][1]] for i in range(3)]
    c0, c1 = solve3(m0), solve3(m1)
    return None if c0 is None or c1 is None else (c0, c1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("field")
    ap.add_argument("--map", action="store_true",
                    help="print the argmax map as a grid of frame patch indices")
    args = ap.parse_args()

    meta, rows = read_field(args.field)
    mw, mh = (int(v) for v in meta.get("marker_grid", "14x14").split("x"))
    fw, fh = (int(v) for v in meta.get("frame_grid", "14x14").split("x"))
    print(f"marker grid {mw}x{mh}   frame grid {fw}x{fh}   "
          f"contextRadius {meta.get('contextRadius')}   "
          f"floor {meta.get('floor')}")

    # frame patch index -> (row, col)
    frames = [(int(r["frame_row"]), int(r["frame_col"])) for r in rows]
    src = [(float(r["marker_col"]), float(r["marker_row"])) for r in rows]
    ctx = [float(r["best_ctx"]) for r in rows]
    gaps = [float(r["gap"]) for r in rows]

    if args.map:
        print("\nargmax map (frame patch index at each marker patch):")
        by_patch = {int(r["marker_patch"]): int(r["frame_patch"]) for r in rows}
        for mr in range(mh):
            cells = []
            for mc in range(mw):
                p = by_patch[mr * mw + mc]
                cells.append(f"{p:>4}" if p >= 0 else "   -")
            print("  " + "".join(cells))
        print()

    n = len(rows)
    distinct = len(set(frames))
    print(f"correspondences          {n} of {mw * mh} marker patches")

    # --- distinct: the marker's footprint in the frame grid, as a fraction of it.
    rows_hit = {r for r, _ in frames if r >= 0}
    cols_hit = {c for _, c in frames if c >= 0}
    bbox_cells = max(1, len(rows_hit)) * max(1, len(cols_hit))
    print(f"distinct frame patches   {distinct}"
          f"   bbox {len(rows_hit)}x{len(cols_hit)} of {fw}x{fh} frame grid"
          f" = {100.0 * bbox_cells / (fw * fh):.0f}% of the frame")

    # --- affine fit, residuals in frame patch cells.
    coeffs = affine_coeffs(src, frames)
    if coeffs is None:
        print("affine fit               degenerate (the map is a straight line)")
        return 1
    (a0, a1, a2), (b0, b1, b2) = coeffs
    res = []
    for (x, y), (u, v) in zip(src, frames):
        pu, pv = a0 * x + a1 * y + a2, b0 * x + b1 * y + b2
        res.append(math.hypot(pu - u, pv - v))
    res_sorted = sorted(res)
    med = res_sorted[len(res_sorted) // 2]
    within = sum(1 for r in res if r <= 1.0) * 100.0 / n
    print(f"affine fit               residual med {med:.2f} frame cells,"
          f" p90 {res_sorted[int(0.9 * n)]:.2f}   "
          f"within the vote's 1.0-cell tolerance: {within:.0f}%")
    print(f"                         (sx {a0:+.3f} sy {b0:+.3f} | "
          f"tx {a1:+.3f} ty {b1:+.3f})")

    # --- how much of the field is signal. If every patch matches at ~the same score
    # then the argmax is choosing between near-identical options, and the map that
    # comes out of it describes the field's drift rather than the marker.
    ctx_sorted = sorted(ctx)
    print(f"context score            med {ctx_sorted[n // 2]:.4f}"
          f"  min {ctx_sorted[0]:.4f}  max {ctx_sorted[-1]:.4f}"
          f"  spread {ctx_sorted[-1] - ctx_sorted[0]:.4f}")
    gaps_sorted = sorted(gaps)
    print(f"best-runner-up gap       med {gaps_sorted[n // 2]:.4f}"
          f"  min {gaps_sorted[0]:.4f}  max {gaps_sorted[-1]:.4f}")
    above = sum(1 for r in rows if int(r["above_floor"]) == 1)
    print(f"cleared the floor        {above} of {n}"
          f"  ({100.0 * above / n:.0f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())