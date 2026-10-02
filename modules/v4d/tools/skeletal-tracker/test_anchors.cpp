// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
//
// Unit test for the MediaPipe BlazePose detector's anchor table.
//
// The table used to be a 601-line generated header plus a Python codegen step.
// It is now computed by a constexpr function, which cannot go stale against the
// model but can still be wrong. This test is what stands in for the codegen
// step: it recomputes the table from an independently written description of
// the model's layout, and pins the few values that a regression would move.

#include <array>
#include <cmath>
#include <cstdio>

#include "skeletal-tracker-anchors.hpp"

namespace {

int failures = 0;

void fail(const char* what, int detail) {
    std::printf("FAIL  %s (%d)\n", what, detail);
    ++failures;
}

// The model's layout, restated here on purpose: the test must not reuse the
// header's own description of it, or it would only check that the header
// computes what it says it computes.
struct Level {
    int grid;
    int stride;
    int heads;
};

constexpr std::array<Level, 3> kLevels = {Level{28, 8, 2}, Level{14, 16, 2}, Level{7, 32, 6}};
constexpr int kInput = 224;

}  // namespace

// --- Compile-time checks ------------------------------------------------------
// The table has to be a constant expression: it is read on the detector's hot
// path, and a run-time generator would rebuild 2254 anchors per frame.

namespace cv {
namespace samples {

static_assert(kPoseDetectorNumAnchors == 2254, "28x28x2 + 14x14x2 + 7x7x6");
static_assert(kPoseDetectorAnchors[2 * 1567] == 0.9821428571f, "level 8 ends at its far corner");
static_assert(kPoseDetectorAnchors[2 * 1568] == 0.0357142857f, "level 16 starts fresh");
static_assert(kPoseDetectorAnchors[2 * 1568] != kPoseDetectorAnchors[0], "levels are distinct");
static_assert(kPoseDetectorAnchors[0] == 0.0178571429f, "first anchor is a cell centre");
static_assert(kPoseDetectorAnchors[2 * 1960] == 0.0714285714f, "level 32 first anchor");
static_assert(kPoseDetectorAnchors[2 * 2253] == 0.9285714286f, "last anchor");

}  // namespace samples
}  // namespace cv

// --- Run-time checks ----------------------------------------------------------

int main() {
    using namespace cv::samples;

    // Every anchor is a cell centre of its level's grid: x varies fastest, and
    // each cell carries one anchor per regression head of that level.
    int i = 0;
    for (const Level& level : kLevels) {
        for (int row = 0; row < level.grid; ++row) {
            for (int col = 0; col < level.grid; ++col) {
                const double x = (level.stride / 2.0 + level.stride * col) / kInput;
                const double y = (level.stride / 2.0 + level.stride * row) / kInput;
                for (int head = 0; head < level.heads; ++head, ++i) {
                    if (std::abs(kPoseDetectorAnchors[2 * i] - x) > 1e-6 ||
                        std::abs(kPoseDetectorAnchors[2 * i + 1] - y) > 1e-6) {
                        fail("anchor is not at its cell centre", i);
                    }
                }
            }
        }
    }
    if (i != kPoseDetectorNumAnchors) {
        fail("anchor count", i);
    }

    // Consecutive anchors of a cell are the same point repeated per head, and
    // x moves fastest within a row.
    for (int a = 0; a + 1 < kPoseDetectorNumAnchors; ++a) {
        const float x = kPoseDetectorAnchors[2 * a], y = kPoseDetectorAnchors[2 * a + 1];
        const float nx = kPoseDetectorAnchors[2 * a + 2], ny = kPoseDetectorAnchors[2 * a + 3];
        if (x == nx && y != ny) {
            fail("x does not vary fastest", a);
        }
        if (x > 1.f || y > 1.f) {
            fail("anchor outside the detector input", a);
        }
    }

    std::printf("%s  %d anchors reproduced from %d grids, %d checks failed\n",
                failures ? "FAIL" : "PASS", kPoseDetectorNumAnchors,
                static_cast<int>(kLevels.size()), failures);
    return failures ? 1 : 0;
}
