#!/usr/bin/env python3
"""Score the dense outline against the ORB outline on the same frames.

The dense path's own numbers cannot say whether its quad is right: a bad fit and a
good one both report dozens of inliers, and both report a convex, plausible,
finite quad. The ORB path is an independent localiser working from whole-frame
keypoints, so on the frames where it commits -- enough matches and a high inlier
ratio -- its quad is a reference the dense quad can be scored against. That
comparison is the measurement this script exists to make; nothing inside the dense
path can supply it.

Per frame where ORB committed and dense also answered:

  area ratio    dense area / ORB area. The signature of the "the outline is just the
                crop" failure is a ratio that is large and varies, not one that is
                reliably near 1.
  centre        distance between the two centres, as a fraction of the frame
                diagonal. A localiser that tracks position but not extent shows a
                small number here and a large one in the area ratio, which is what
                separates the two defects.
  IoU           intersection over union of the two quads, which punishes both
                mistakes at once and is the number to optimise.

Areas and the intersection are computed after clipping to the frame, because a
quad that leaves the frame is not describing an outline that can be drawn -- and
comparing clipped areas against unclipped ones would flatter it.

usage: compare-outlines.py DENSE.csv ORB.csv [--frame W H]
"""

import argparse
import csv
import math
import sys


def signed_area(pts):
    a = 0.0
    for i in range(len(pts)):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % len(pts)]
        a += x0 * y1 - x1 * y0
    return a * 0.5


def area(pts):
    return abs(signed_area(pts))


def corners(r):
    return [(float(r[f"x{i}"]), float(r[f"y{i}"])) for i in range(4)]


def centre(pts):
    n = len(pts)
    return (sum(p[0] for p in pts) / n, sum(p[1] for p in pts) / n)


def clip(subject, planes):
    """Sutherland-Hodgman. `planes` is [(nx, ny, d)]: keep nx*x + ny*y >= d."""
    out = subject
    for nx, ny, d in planes:
        if not out:
            return []
        cur, out = out, []
        for i in range(len(cur)):
            a, b = cur[i], cur[(i + 1) % len(cur)]
            da = nx * a[0] + ny * a[1] - d
            db = nx * b[0] + ny * b[1] - d
            if da >= 0:
                out.append(a)
            if (da >= 0) != (db >= 0):
                t = da / (da - db)
                out.append((a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])))
    return out


def frame_planes(fw, fh):
    # Keep nx*x + ny*y >= d, so a "less than" bound is a negated normal *and* a
    # negated offset. Negating only the normal gives x <= -fw.
    return [(1.0, 0.0, 0.0), (-1.0, 0.0, -float(fw)),
            (0.0, 1.0, 0.0), (0.0, -1.0, -float(fh))]


def quad_planes(quad):
    """Half-planes of a convex quad, oriented so `quad` lies inside all of them."""
    if signed_area(quad) < 0:
        quad = list(reversed(quad))
    return [(quad[i][1] - quad[(i + 1) % 4][1],
             quad[(i + 1) % 4][0] - quad[i][0],
             quad[i][1] * quad[(i + 1) % 4][0] - quad[(i + 1) % 4][1] * quad[i][0])
            for i in range(4)]


def load(path):
    return {int(r["frame"]): r for r in csv.DictReader(open(path))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dense_csv")
    ap.add_argument("orb_csv")
    ap.add_argument("--frame", type=int, nargs=2, default=(480, 854),
                    metavar=("W", "H"))
    args = ap.parse_args()
    fw, fh = args.frame
    diag = math.hypot(fw, fh)
    box = frame_planes(fw, fh)

    dense, orb = load(args.dense_csv), load(args.orb_csv)

    print(f"{'frame':>6} {'denseA%':>8} {'orbA%':>7} {'area x':>7} "
          f"{'ctr%':>6} {'IoU':>6} {'out':>4} {'orbInl':>7} {'orbMtch':>7}")
    ratios, centres, ious, oob = [], [], [], 0
    for f in sorted(set(dense) & set(orb)):
        d, o = dense[f], orb[f]
        # ORB must have committed on its own, and the dense row must be the dense
        # localiser's answer. Otherwise the pair says nothing about the dense fit.
        if o["by"] != "2" or d["by"] != "1":
            continue
        dc, oc = corners(d), corners(o)
        outside = any(not (0 <= x <= fw and 0 <= y <= fh) for x, y in dc)
        oob += 1 if outside else 0
        dclip, oclip = clip(dc, box), clip(oc, box)
        da = area(dclip) if len(dclip) >= 3 else 0.0
        oa = area(oclip) if len(oclip) >= 3 else 0.0
        if oa <= 1e-6 or da <= 1e-6:
            continue
        inter = clip(dclip, quad_planes(oclip))
        ia = area(inter) if len(inter) >= 3 else 0.0
        union = da + oa - ia
        dctr, octr = centre(dc), centre(oc)
        ratios.append(da / oa)
        centres.append(math.dist(dctr, octr) / diag)
        ious.append(ia / union if union > 1e-9 else 0.0)
        print(f"{f:>6} {100 * da / (fw * fh):>8.1f} {100 * oa / (fw * fh):>7.1f} "
              f"{da / oa:>7.2f} {100 * math.dist(dctr, octr) / diag:>6.1f} "
              f"{ious[-1]:>6.2f} {'yes' if outside else 'no':>4} "
              f"{float(o['orb_inlier_ratio']):>7.2f} {o['orb_matches']:>7}")

    if not ratios:
        print("\nno frame where ORB committed and dense also answered",
              file=sys.stderr)
        return 1

    def med(v):
        s = sorted(v)
        return s[len(s) // 2]

    print()
    print(f"frames compared          {len(ratios)}"
          f"   (dense quads leaving the frame: {oob})")
    print(f"area ratio dense/ORB     med {med(ratios):.2f}"
          f"  min {min(ratios):.2f}  max {max(ratios):.2f}")
    print(f"|centre offset|          med {100 * med(centres):.1f}%"
          f"  max {100 * max(centres):.1f}%  of the frame diagonal")
    print(f"IoU                      med {med(ious):.2f}"
          f"  min {min(ious):.2f}  max {max(ious):.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())