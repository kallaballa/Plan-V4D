#!/usr/bin/env python3
"""Check that an Android screencap is a live rendered demo rather than a blank window.

    adb exec-out screencap -p > screen.png
    tools/android/check-frame.py screen.png

A V4D window that never rendered is a flat fill, and one that renders a camera
frame has structure: per-pixel spread, edges, and motion between two captures.
This exists because the demo can look identical on screen whether the camera
delivered frames or not, and because neither this model nor a build log can tell
the difference -- only pixels can.
"""
import sys

import cv2
import numpy as np


def load(path):
    img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if img is None:
        raise SystemExit(f"{path}: not an image (empty file?)")
    return img


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__.strip())
    path = argv[1]
    img = load(path)
    bgr = img[:, :, :3] if img.ndim == 3 else cv2.cvtColor(img, cv2.COLOR_GRAY2BGR)
    gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY).astype(np.float32)

    # The status bar and any system UI are not the demo; crop them away.
    h = gray.shape[0]
    body = gray[int(h * 0.08):int(h * 0.92), :]

    edges = cv2.Laplacian(body, cv2.CV_32F).var()
    hist = cv2.calcHist([body.astype(np.uint8)], [0], None, [256], [0, 256]).ravel()
    hist /= hist.sum()
    entropy = float(-(hist[hist > 0] * np.log2(hist[hist > 0])).sum())

    print(f"{path}: {img.shape[1]}x{img.shape[0]}")
    print(f"  mean            {body.mean():7.2f}   (a blank window is flat)")
    print(f"  stddev          {body.std():7.2f}")
    print(f"  min/max         {body.min():.0f} / {body.max():.0f}")
    print(f"  edge energy     {edges:7.2f}   (Laplacian variance)")
    print(f"  entropy         {entropy:7.2f} bits")

    verdict = []
    if body.std() < 2.0:
        verdict.append("FLAT: this looks like an unrendered/blank window")
    if edges < 1.0:
        verdict.append("NO EDGES: nothing has been drawn")
    if entropy < 1.0:
        verdict.append("LOW ENTROPY: too few distinct tones to be a camera image")

    if len(argv) > 2:
        other = load(argv[2])
        ob = cv2.cvtColor(other[:, :, :3], cv2.COLOR_BGR2GRAY).astype(np.float32)
        ob = ob[int(ob.shape[0] * 0.08):int(ob.shape[0] * 0.92), :]
        diff = cv2.absdiff(body, ob)
        print(f"  vs {argv[2]}")
        print(f"  mean abs diff   {diff.mean():7.2f}")
        print(f"  changed pixels  {float((diff > 8).mean()) * 100:6.1f}%")
        if float((diff > 8).mean()) < 0.005:
            verdict.append("STATIC: the two captures are the same image")

    print()
    if verdict:
        for v in verdict:
            print(f"  !! {v}")
        return 1
    print("  OK: the window has real, changing content in it")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))