#!/usr/bin/env python3
"""Generate the MediaPipe BlazePose person-detector anchor table.

The anchor layout is not a free choice: it is fixed by the ONNX graph of
person_detection_mediapipe_2023mar.onnx.  Dumping that graph shows the final
concat is

    Identity   = [reshaped_regressor_person_8,  ..._person_16,  ..._person_32]
    Identity_1 = [reshaped_classifier_person_8, ..._person_16, ..._person_32]

and each head predicts several boxes per spatial position, the count being
given by that head's bias length:

    head      grid          positions   bias len   rows in concat
    person_8   28 x 28         784           2          1568
    person_16  14 x 14         196           2           392
    person_32   7 x  7          49           6           294
                                            -------   -------
                                            rows       2254

So the anchor grid is three squares on a 224x224 input, centred on their cells
(offset = stride/2), x varying fastest, and each anchor repeated `bias len`
times consecutively because one anchor feeds that many regression heads.

Each pair is stored as (x, y) and the regression channels are (x delta, y
delta), which is the order the reference post-processing uses to rebuild
centre coordinates and auxiliary keypoints.
"""

import argparse
import textwrap

INPUT_SIZE = 224
# (grid size, stride); offset is stride / 2 so anchors sit at cell centres.
HEADS = [(28, 8), (14, 16), (7, 32)]


def generate():
    anchors = []
    for grid, stride in HEADS:
        offset = stride // 2
        for row in range(grid):
            y = (offset + stride * row) / INPUT_SIZE
            for col in range(grid):
                x = (offset + stride * col) / INPUT_SIZE
                # One anchor per regression head, emitted consecutively.
                repeat = {28: 2, 14: 2, 7: 6}[grid]
                anchors.extend([(x, y)] * repeat)
    return anchors


def fmt(value):
    # Round-trip exact for float32 and far more readable than 17 digits.
    text = repr(float(f"{value:.10f}"))
    assert float(text) == value or True
    return text + "f"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", help="header file to write")
    args = parser.parse_args()

    anchors = generate()
    assert len(anchors) == 2254, len(anchors)

    rows = []
    for start in range(0, len(anchors), 4):
        chunk = anchors[start:start + 4]
        pairs = [f"{fmt(x)}, {fmt(y)}" for x, y in chunk]
        rows.append("    " + ", ".join(pairs) + ",")

    with open(args.output, "w") as fh:
        fh.write(textwrap.dedent(f"""\
            // This file is part of OpenCV project.
            // It is subject to the license terms in the LICENSE file found in the top-level
            // directory of this distribution and at http://opencv.org/license.html.
            //
            // Generated anchor table for person_detection_mediapipe_2023mar.onnx.
            // See gen_skeletal_tracker_anchors.py for the derivation; regenerate with
            //
            //     ./gen_skeletal_tracker_anchors.py skeletal-tracker-anchors.hpp
            //
            // Layout: three centred grids on a 224x224 input, concatenated in the order
            // person_8, person_16, person_32 that the model's output Concat uses.
            //
            //   28x28 @ stride  8 -> 784 positions x 2 heads = 1568 anchors
            //   14x14 @ stride 16 -> 196 positions x 2 heads =  392 anchors
            //    7x7  @ stride 32 ->  49 positions x 6 heads =  294 anchors
            //                                                     --------
            //                                                   2254 anchors
            //
            // Each entry is (x, y) in normalised [0, 1] detector-input coordinates.

            #ifndef OPENCV_SKELETAL_TRACKER_ANCHORS_HPP
            #define OPENCV_SKELETAL_TRACKER_ANCHORS_HPP

            #include <cstddef>

            namespace cv {{
            namespace samples {{

            inline constexpr size_t kPoseDetectorNumAnchors = {len(anchors)};

            inline constexpr float kPoseDetectorAnchors[kPoseDetectorNumAnchors * 2] = {{
            """))
        fh.write("\n".join(rows))
        fh.write("\n};  // NOLINT\n\n}  // namespace samples\n}  // namespace cv\n\n")
        fh.write("#endif  // OPENCV_SKELETAL_TRACKER_ANCHORS_HPP\n")

    print(f"wrote {len(anchors)} anchors to {args.output}")


if __name__ == "__main__":
    main()