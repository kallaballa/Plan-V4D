#!/usr/bin/env python3
"""Check the channel order a cv2.VideoCapture gives back for a video file.

    tools/android/check-capture-order.py

Source::makeDefault opens the Android camera with an explicit fourCC, which is
only the right choice if a Source is supposed to deliver BGR: the swap in
detail::SourceContext (COLOR_RGB2BGRA) is only correct if the incoming frame is
BGR, which is what the file backend produces. Getting that assumption wrong
shows up as red and blue exchanged in every pixel, and not as any error, so it
is worth measuring rather than reasoning about.

The test writes a frame whose channels are deliberately unequal and known, reads
it back, and reports which channel ended up where. If VideoWriter takes BGR and
hands it straight to the encoder, then writing BGR-ordered bytes for "red" and
reading them back as BGR is a no-op -- which is exactly what the round trip
below shows.

    Channel under test   interpretation if it comes back equal
    ------------------   ------------------------------------
    0                     the file backend gives BGR (channel 0 = blue)
    2                     the file backend gives RGB (channel 0 = red)
"""
import os
import tempfile

import cv2
import numpy as np

# Written as BGR, i.e. (blue=11, green=57, red=203): a colour with no two
# channels close together, so a transposition cannot go unnoticed.
BGR = (11, 57, 203)


def main():
    tmp = tempfile.mkdtemp(prefix="v4d-chorder-")
    path = os.path.join(tmp, "probe.avi")
    fourcc = cv2.VideoWriter_fourcc(*"MJPG")

    writer = cv2.VideoWriter(path, fourcc, 25.0, (64, 48))
    if not writer.isOpened():
        raise SystemExit(f"cannot write {path}; is FFmpeg in this cv2 build?")
    frame = np.full((48, 64, 3), BGR, dtype=np.uint8)
    for _ in range(5):
        writer.write(frame)
    writer.release()

    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        raise SystemExit(f"cannot read back {path}")
    ok, got = cap.read()
    cap.release()
    if not ok:
        raise SystemExit("VideoCapture.read() failed on a file just written")

    px = got[0, 0].tolist()
    print(f"wrote BGR {BGR} (blue=11, green=57, red=203)")
    print(f"read back    {px}")
    print(f"max error vs BGR order: {max(abs(px[i] - BGR[i]) for i in range(3))}")

    os.remove(path)
    os.rmdir(tmp)

    if max(abs(px[i] - BGR[i]) for i in range(3)) <= 4:
        print("\n=> the capture backend delivers BGR: a Source must hand V4D BGR,")
        print("   and the Android camera must be asked for BGRA.")
        return 0
    if max(abs(px[i] - BGR[::-1][i]) for i in range(3)) <= 4:
        print("\n=> the capture backend delivers RGB (MJPG is YUV and is *not*")
        print("   converted on read), so a Source must hand V4D RGB.")
        return 2
    print("\n=> unrecognised channel order; the probe video may not have survived")
    print("   the codec. Check the values above by hand.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())