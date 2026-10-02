// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
//
// The MediaPipe BlazePose person detector is an anchor-point detector: the
// graph bakes in the cell offsets, so the anchors below are only used to turn
// the predicted (x, y) *deltas* back into absolute input coordinates.
//
// The layout is not a free choice -- it is fixed by the ONNX graph of
// person_detection_mediapipe_2023mar.onnx. Dumping that graph shows the final
// concat is
//
//     Identity   = [reshaped_regressor_person_8,  ..._person_16,  ..._person_32]
//     Identity_1 = [reshaped_classifier_person_8, ..._person_16, ..._person_32]
//
// and each head predicts several boxes per spatial position, the count being
// given by that head's bias length. So the anchors are three centred squares on
// a 224x224 input (offset = stride/2, x varying fastest), each position
// repeated once per regression head of that level, concatenated in the order
// above:
//
//   person_8 : 28x28 cells, stride  8, 2 heads -> 1568 anchors
//   person_16: 14x14 cells, stride 16, 2 heads ->  392 anchors
//   person_32:  7x7 cells, stride 32, 6 heads ->  294 anchors
//                                                  -------
//                                                2254 anchors
//
// Being that regular, the table does not need to be generated and shipped: the
// table below is computed by a constexpr function, so it cannot go stale
// against the model. `tools/skeletal-tracker/test_anchors.cpp` checks it,
// including against float values taken from the generated table this replaced.

#ifndef OPENCV_SKELETAL_TRACKER_ANCHORS_HPP
#define OPENCV_SKELETAL_TRACKER_ANCHORS_HPP

#include <array>

namespace cv {
namespace samples {

// One detector level: a grid of `grid` x `grid` cells, `stride` pixels apart on
// the 224x224 detector input, with `heads` anchors per cell position (one per
// regression head, which share a cell centre).
struct PoseDetectorLevel {
    int grid;
    int stride;
    int heads;
};

inline constexpr int kPoseDetectorInputSize = 224;
inline constexpr std::array<PoseDetectorLevel, 3> kPoseDetectorLevels = {
    PoseDetectorLevel{28, 8, 2}, PoseDetectorLevel{14, 16, 2}, PoseDetectorLevel{7, 32, 6}};

// Rows of the detector's output, i.e. the anchors in the model's concat order.
inline constexpr int kPoseDetectorNumAnchors = 28 * 28 * 2 + 14 * 14 * 2 + 7 * 7 * 6;

// (x, y) pairs, normalised to [0, 1] detector-input coordinates, as two floats
// per anchor.
inline constexpr std::array<float, 2 * kPoseDetectorNumAnchors> makePoseDetectorAnchors() {
    std::array<float, 2 * kPoseDetectorNumAnchors> out{};
    int i = 0;
    for (const PoseDetectorLevel& level : kPoseDetectorLevels) {
        // Anchors sit at cell centres, so the first one is half a stride in.
        const float offset = static_cast<float>(level.stride / 2);
        for (int row = 0; row < level.grid; ++row) {
            const float y = (offset + static_cast<float>(level.stride * row)) /
                            static_cast<float>(kPoseDetectorInputSize);
            for (int col = 0; col < level.grid; ++col) {
                const float x = (offset + static_cast<float>(level.stride * col)) /
                                static_cast<float>(kPoseDetectorInputSize);
                // One anchor per regression head, emitted consecutively.
                for (int head = 0; head < level.heads; ++head) {
                    out[2 * i] = x;
                    out[2 * i + 1] = y;
                    ++i;
                }
            }
        }
    }
    return out;
}

inline constexpr std::array<float, 2 * kPoseDetectorNumAnchors> kPoseDetectorAnchors =
    makePoseDetectorAnchors();

}  // namespace samples
}  // namespace cv

#endif  // OPENCV_SKELETAL_TRACKER_ANCHORS_HPP
