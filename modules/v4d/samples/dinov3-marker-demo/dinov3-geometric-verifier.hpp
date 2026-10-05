// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The geometric half of the DINOv3 Marker Demo.
//
// A DINOv3 descriptor says "this looks like the harddisk" but not "and it is
// here, at this angle, that big". That is enough to recognise a marker in a
// centre crop, and it is not enough to reject a lookalike or to draw an outline
// around the marker. So every accepted embedding match is confirmed with local
// features:
//
//   1. match the marker's stored keypoints against keypoints of the current
//      frame (knn + Lowe ratio test),
//   2. fit a homography from the marker's crop to the frame with a robust
//      estimator and count the inliers,
//   3. project the marker's four crop corners through that homography, which is
//      what the HUD outlines.
//
// Step 3 is what upgrades the demo from "there is a marker somewhere" to "there
// is a marker, here".
//
// Note on the feature type: the playbook specified AKAZE, but this OpenCV 5.x
// tree has no AKAZE at all (no akaze.cpp, no class), so ORB is the default and
// SIFT the alternative. ORB gives binary descriptors and NORM_HAMMING, which is
// several times faster than SIFT at this scale and plenty for a textured object
// filling the crop.
//
// Like the embedder and the database, this header deliberately knows nothing
// about V4D, so the head-less self-test can drive it too.

#ifndef OPENCV_DINOV3_GEOMETRIC_VERIFIER_HPP
#define OPENCV_DINOV3_GEOMETRIC_VERIFIER_HPP

#include "dinov3-marker-database.hpp"

#include <opencv2/core.hpp>
#include <opencv2/features.hpp>

#include <string>
#include <vector>

namespace cv {
namespace samples {
namespace dinov3 {

enum class FeatureType {
  Orb,  // binary descriptors, NORM_HAMMING -- the default
  Sift  // float descriptors, NORM_L2
};

std::string toString(FeatureType type);
bool featureTypeFromString(const std::string &name, FeatureType &type);

struct VerifierOptions {
  FeatureType type = FeatureType::Orb;

  // Cap on the detector's own feature budget, and on the long side of the image
  // it is allowed to work on. Both exist to bound the cost of verification: it
  // runs on every processed frame, unlike the embedding which can be throttled
  // to every Nth frame without losing much.
  int maxFeatures = 1500;
  int maxWorkDim = 960;

  // The frame is detected at each of these factors in turn and the best attempt
  // wins. ORB has only a handful of octaves of scale invariance, so a marker
  // that is twice as close (or twice as far) as the reference photo simply
  // produces no matches -- measured directly on the marker video: frames at a
  // matching scale gave 124-313 matches at a 0.66-0.79 inlier ratio, frames off
  // it gave 10-80 matches at 0.11-0.36. Since the demo's whole point is
  // recognising a marker "from various angles and distances", one scale is not
  // enough.
  std::vector<double> scales{0.5, 1.0, 2.0};

  // Lowe ratio test on the 1st/2nd-best descriptor distance.
  float ratioThreshold = 0.75f;

  // A match set this small is noise, and a homography fitted to noise is
  // arbitrary, so require this many surviving matches before fitting at all.
  int minMatches = 12;

  // ... and then this many of them must agree with the fitted homography.
  int minInliers = 10;
  float minInlierRatio = 0.35f;

  double ransacReprojThreshold = 4.0;
  bool enabled = true;
};

struct VerifyResult {
  bool verified = false;
  float inlierRatio = 0.0f;
  int matches = 0;
  int inliers = 0;

  // The marker's crop corners, mapped into full-frame pixel coordinates. Empty
  // unless verification succeeded.
  std::vector<Point2f> corners;

  double lastMs = 0.0;
  std::string error; // empty unless something went structurally wrong
};

class GeometricVerifier {
public:
  explicit GeometricVerifier(const VerifierOptions &options = VerifierOptions());

  const VerifierOptions &options() const;
  void setOptions(const VerifierOptions &options);

  // Keypoints and descriptors of a BGR crop, for storage in a MarkerRecord.
  // `space` receives the size of the image the points actually live in, which
  // is the crop scaled down to maxWorkDim -- record that as the marker's
  // cropSize and the corners below stay consistent with the keypoints.
  bool extract(const Mat &bgr, LocalFeatures &features, Size2f &space,
               std::string &error);

  // Confirms `marker` against `frame` and, on success, fills in `result`.
  // A structural problem (too few features, descriptor type mismatch, a
  // degenerate fit) leaves verified=false and explains itself in `error`;
  // an ordinary "this is not the marker" is verified=false with no error.
  bool verify(const MarkerRecord &marker, InputArray frame,
              VerifyResult &result);

private:
  // Resizes `bgr` so its long side is at most maxWorkDim, reporting the scale
  // that was applied (1.0 when nothing happened).
  Mat prepared(const Mat &bgr, double &scale) const;
  bool detect(const Mat &gray, LocalFeatures &features, std::string &error);

  // One pass at one detection scale: match, fit, project. Fills in matches,
  // inliers, inlierRatio and corners in the coordinates of the image it was
  // handed, and returns whether a homography was fitted at all -- it
  // deliberately does not apply the acceptance thresholds, so that the caller
  // can pick the most promising scale first and judge it once.
  bool attemptAtScale(const MarkerRecord &marker, const Mat &gray, double scale,
                      VerifyResult &out);

  VerifierOptions options_;
  Ptr<Feature2D> detector_;
  TickMeter timer_;
  Mat gray_;
  std::vector<KeyPoint> keypoints_;
};

} // namespace dinov3
} // namespace samples
} // namespace cv

#endif // OPENCV_DINOV3_GEOMETRIC_VERIFIER_HPP