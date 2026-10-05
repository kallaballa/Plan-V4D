// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include "dinov3-geometric-verifier.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

// findHomography() moved out of calib3d and into the geometry module in this
// OpenCV; perspectiveTransform() stayed in core. That is why the sample links
// opencv_geometry -- see the add_multisource_sample() line in
// modules/v4d/CMakeLists.
#include <opencv2/geometry/3d.hpp>

namespace cv {
namespace samples {
namespace dinov3 {

std::string toString(FeatureType type) {
  return type == FeatureType::Sift ? "SIFT" : "ORB";
}

bool featureTypeFromString(const std::string &name, FeatureType &type) {
  if (name == "orb" || name == "ORB") {
    type = FeatureType::Orb;
    return true;
  }
  if (name == "sift" || name == "SIFT") {
    type = FeatureType::Sift;
    return true;
  }
  return false;
}

namespace {

int normFor(FeatureType type) {
  return type == FeatureType::Sift ? NORM_L2 : NORM_HAMMING;
}

// The two corners of a crop, clockwise from the top-left. This is the space the
// marker's keypoints were measured in, so the homography takes them straight to
// the frame.
std::vector<Point2f> cropCorners(const Size2f &size) {
  return {
      Point2f(0.0f, 0.0f), Point2f(size.width, 0.0f),
      Point2f(size.width, size.height), Point2f(0.0f, size.height)};
}

// Guards the projected outline before anyone draws it. A homography can satisfy
// the inlier count and still produce a degenerate quad -- observed directly:
// four identical corners -- so the area and the diagonal both have to be real.
bool plausibleQuad(const std::vector<Point2f> &corners, const Size &frameSize) {
  if (corners.size() != 4)
    return false;

  double area = 0.0;
  for (int i = 0; i < 4; ++i) {
    const Point2f &a = corners[i];
    const Point2f &b = corners[(i + 1) % 4];
    area += (double)a.x * b.y - (double)b.x * a.y;
  }
  area = std::abs(area) * 0.5;

  // Something has to have an area, and it has to be a sensible fraction of the
  // frame rather than a speck or the whole image many times over.
  const double frameArea = (double)frameSize.width * frameSize.height;
  if (area < 400.0 || area > 4.0 * frameArea)
    return false;

  // No two corners may coincide.
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j)
      if (std::abs(corners[i].x - corners[j].x) < 1.0f &&
          std::abs(corners[i].y - corners[j].y) < 1.0f)
        return false;
  return true;
}

} // namespace

GeometricVerifier::GeometricVerifier(const VerifierOptions &options) {
  setOptions(options);
}

void GeometricVerifier::setOptions(const VerifierOptions &options) {
  options_ = options;
  // Both detectors are created lazily-by-construction rather than shared: ORB
  // and SIFT have different create() signatures, so a branch is cheaper to read
  // than a variant type.
  if (options_.type == FeatureType::Sift) {
    detector_ = SIFT::create(options_.maxFeatures);
  } else {
    detector_ = ORB::create(options_.maxFeatures);
  }
}

const VerifierOptions &GeometricVerifier::options() const { return options_; }

Mat GeometricVerifier::prepared(const Mat &bgr, double &scale) const {
  scale = 1.0;
  const int longSide = std::max(bgr.cols, bgr.rows);
  // maxWorkDim <= 0 means "use the image as it is". Guarding it explicitly
  // matters: the alternative is a scale of 0 and a resize() assertion.
  if (options_.maxWorkDim <= 0 || longSide <= options_.maxWorkDim ||
      longSide <= 0)
    return bgr;

  scale = (double)options_.maxWorkDim / (double)longSide;
  Mat small;
  resize(bgr, small, Size(cvRound(bgr.cols * scale), cvRound(bgr.rows * scale)),
         0.0, 0.0, INTER_AREA);
  return small;
}

bool GeometricVerifier::detect(const Mat &gray, LocalFeatures &features,
                               std::string &error) {
  keypoints_.clear();
  features.descriptors.release();
  features.points.clear();

  try {
    detector_->detectAndCompute(gray, noArray(), keypoints_, features.descriptors);
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }

  features.points.reserve(keypoints_.size());
  for (const KeyPoint &kp : keypoints_)
    features.points.emplace_back(kp.pt);
  return true;
}

bool GeometricVerifier::extract(const Mat &bgr, LocalFeatures &features,
                                Size2f &space, std::string &error) {
  if (bgr.empty()) {
    error = "empty crop";
    return false;
  }
  if (bgr.channels() == 1)
    bgr.copyTo(gray_);
  else if (bgr.channels() == 4)
    cvtColor(bgr, gray_, COLOR_BGRA2GRAY);
  else
    cvtColor(bgr, gray_, COLOR_BGR2GRAY);

  double scale = 1.0;
  const Mat work = prepared(gray_, scale);
  if (!detect(work, features, error))
    return false;

  space = Size2f((float)work.cols, (float)work.rows);
  return true;
}

bool GeometricVerifier::attemptAtScale(const MarkerRecord &marker,
                                       const Mat &gray, double scale,
                                       VerifyResult &out) {
  Mat scaled;
  if (scale != 1.0) {
    resize(gray, scaled,
           Size(cvRound(gray.cols * scale), cvRound(gray.rows * scale)), 0.0,
           0.0, scale > 1.0 ? INTER_CUBIC : INTER_AREA);
  } else {
    scaled = gray;
  }

  // The detector is shared mutable state, so this cannot run concurrently; the
  // verifier is documented as single-threaded for that reason.
  LocalFeatures frameFeatures;
  std::string error;
  if (!detect(scaled, frameFeatures, error) || frameFeatures.empty())
    return false;

  // Lowe's ratio test: a match only survives if the best candidate is clearly
  // better than the runner-up. This is what throws out matches against
  // repetitive texture, which a harddisk platter full of concentric circles has
  // plenty of.
  std::vector<std::vector<DMatch>> knn;
  try {
    BFMatcher matcher(normFor(options_.type));
    matcher.knnMatch(marker.features.descriptors, frameFeatures.descriptors, knn,
                     2);
  } catch (const std::exception &) {
    return false;
  }

  std::vector<Point2f> markerPoints, framePoints;
  markerPoints.reserve(knn.size());
  framePoints.reserve(knn.size());
  for (const std::vector<DMatch> &pair : knn) {
    if (pair.size() < 2)
      continue;
    const DMatch &best = pair[0];
    const DMatch &second = pair[1];
    if (best.distance < options_.ratioThreshold * second.distance) {
      markerPoints.push_back(marker.features.points[(size_t)best.queryIdx]);
      framePoints.push_back(frameFeatures.points[(size_t)best.trainIdx]);
    }
  }

  out.matches = (int)markerPoints.size();
  if (out.matches < 4) // below any sane minMatches; not worth fitting
    return false;

  // MAGSAC++ scores better than RANSAC on the skewed, repetitive, partially
  // occluded views a handheld marker video contains, but the plain estimator
  // stays as a fallback in case a build lacks it.
  Mat inlierMask;
  Mat h;
  try {
    h = findHomography(markerPoints, framePoints, USAC_MAGSAC,
                       options_.ransacReprojThreshold, inlierMask);
  } catch (const std::exception &) {
    try {
      h = findHomography(markerPoints, framePoints, RANSAC,
                         options_.ransacReprojThreshold, inlierMask);
    } catch (const std::exception &) {
      return false;
    }
  }

  if (h.empty() || h.rows != 3 || h.cols != 3)
    return false;

  if (!inlierMask.empty() && inlierMask.type() == CV_8U)
    out.inliers = countNonZero(inlierMask);
  else
    out.inliers = out.matches;
  out.inlierRatio =
      (float)out.inliers / (float)std::max(1, out.matches);

  std::vector<Point2f> corners;
  perspectiveTransform(cropCorners(marker.cropSize), corners, h);

  // Back out of the detection scale into `gray`'s coordinates; the caller still
  // has to undo prepared()'s downscale.
  if (scale != 1.0)
    for (Point2f &corner : corners)
      corner *= (float)(1.0 / scale);

  out.corners = std::move(corners);
  return true;
}

bool GeometricVerifier::verify(const MarkerRecord &marker, InputArray frame,
                               VerifyResult &result) {
  result = VerifyResult();

  if (!options_.enabled) {
    result.error = "verification is off";
    return false;
  }
  if (marker.features.empty() || marker.cropSize.width <= 0 ||
      marker.cropSize.height <= 0) {
    result.error = "the marker has no stored features";
    return false;
  }

  const Mat frameMat = frame.getMat();
  if (frameMat.empty()) {
    result.error = "empty frame";
    return false;
  }

  // A marker saved with ORB descriptors cannot be matched against SIFT
  // descriptors: the matcher's norm and the descriptor width both assume one or
  // the other. Refuse instead of throwing out of a graph node.
  const int expectedType =
      marker.features.descriptors.type() == CV_8U ? CV_8U : CV_32F;
  if (expectedType != (options_.type == FeatureType::Sift ? CV_32F : CV_8U)) {
    result.error = "the marker was registered with a different feature type";
    return false;
  }

  timer_.start();

  Mat gray;
  if (frameMat.channels() == 1)
    frameMat.copyTo(gray);
  else if (frameMat.channels() == 4)
    cvtColor(frameMat, gray, COLOR_BGRA2GRAY);
  else
    cvtColor(frameMat, gray, COLOR_BGR2GRAY);

  double preparedScale = 1.0;
  const Mat work = prepared(gray, preparedScale);

  // Try every scale and keep the attempt with the most geometric agreement.
  // Judging each attempt against the thresholds in isolation would let a weak
  // scale blank out a strong one.
  bool anyFit = false;
  VerifyResult best;
  for (double scale : options_.scales) {
    VerifyResult attempt;
    if (!attemptAtScale(marker, work, scale, attempt))
      continue;
    if (!anyFit || attempt.inliers > best.inliers ||
        (attempt.inliers == best.inliers &&
         attempt.inlierRatio > best.inlierRatio)) {
      anyFit = true;
      best = std::move(attempt);
    }
  }

  if (!anyFit) {
    timer_.stop();
    result.lastMs = timer_.getLastTimeMilli();
    result.matches = 0;
    result.error = "no features in the current frame";
    return false;
  }

  result.matches = best.matches;
  result.inliers = best.inliers;
  result.inlierRatio = best.inlierRatio;

  // The attempts worked in prepared(gray) coordinates; the HUD wants real frame
  // pixels, so undo prepared()'s downscale too.
  result.corners = best.corners;
  if (preparedScale != 1.0)
    for (Point2f &corner : result.corners)
      corner *= (float)(1.0 / preparedScale);

  // A fit can satisfy the inlier test and still collapse: the observed SIFT run
  // produced four identical corners at (390,465). Reject anything that is not a
  // real quad before it reaches the HUD as a marker outline.
  if (!plausibleQuad(result.corners, frameMat.size())) {
    timer_.stop();
    result.lastMs = timer_.getLastTimeMilli();
    result.corners.clear();
    result.error = "the fitted marker is degenerate";
    return false;
  }

  if (result.matches < options_.minMatches) {
    timer_.stop();
    result.lastMs = timer_.getLastTimeMilli();
    result.error.clear(); // not an error: this simply is not the marker
    return false;
  }
  if (result.inliers < options_.minInliers ||
      result.inlierRatio < options_.minInlierRatio) {
    timer_.stop();
    result.lastMs = timer_.getLastTimeMilli();
    result.error.clear(); // matches existed but they did not agree on one marker
    return false;
  }

  // A marker held partly out of frame is still a detection, so only reject when
  // the whole outline misses the frame. The reference crop is usually wider than
  // the marker itself, so a corner landing outside says nothing.
  const Rect bounds(0, 0, frameMat.cols, frameMat.rows);
  int inside = 0;
  for (const Point2f &corner : result.corners)
    if (bounds.contains(Point(cvRound(corner.x), cvRound(corner.y))))
      ++inside;
  if (inside == 0) {
    timer_.stop();
    result.lastMs = timer_.getLastTimeMilli();
    result.corners.clear();
    result.error = "the marker is outside the frame";
    return false;
  }

  result.verified = true;

  timer_.stop();
  result.lastMs = timer_.getLastTimeMilli();
  return true;
}

} // namespace dinov3
} // namespace samples
} // namespace cv