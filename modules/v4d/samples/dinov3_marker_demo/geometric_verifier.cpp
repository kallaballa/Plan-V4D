// Dense localisation from DINOv3 patch tokens, ported from the sibling demo's
// proven matcher (modules/v4d/samples/dinov3-marker-demo/dinov3-patch-matcher.cpp).
// The header explains why each stage exists; this file is those stages in order:
//
//   cosineMatrix()   one gemm: cos(marker patch, frame patch) for all pairs
//   contextFilter()  a separable binomial window over both patch grids
//   top2()           best and runner-up, with non-maximum suppression
//   buildCandidates() the loose set: every patch that cleared the floor
//   vote()           deterministic 4-point hypotheses, scored over all candidates
//   refine()         MAGSAC++ and a robust affine fit, then model selection
//   project()        the crop corners through the fit, and the sanity checks
//
// The port is not a copy: the sibling keeps its own PatchGrid/DenseReference types
// and its own diagnostics, whereas this file sits inside a V4D sample that owns a
// MarkerRecord and a temporal filter. What was taken is the algorithm, and with it
// the measurements that justify it -- in particular that the correspondence stage
// must not be a plain ratio test, which is what the first attempt at this was and
// what produced zero outlines on every frame.

#include "geometric_verifier.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr double kHalfPi = 1.57079632679489661923;

// Signed area of a polygon, positive when the corners run counter-clockwise in an
// image coordinate system (y-down, so "clockwise" on screen).
double signedArea(const std::vector<cv::Point2f>& q) {
  double a = 0.0;
  for (size_t i = 0; i < q.size(); ++i) {
    const cv::Point2f& p0 = q[i];
    const cv::Point2f& p1 = q[(i + 1) % q.size()];
    a += static_cast<double>(p0.x) * p1.y - static_cast<double>(p1.x) * p0.y;
  }
  return a * 0.5;
}

cv::Point2f centroid(const std::vector<cv::Point2f>& q) {
  cv::Point2f c(0.0f, 0.0f);
  for (const auto& p : q)
    c += p;
  if (!q.empty())
    c *= 1.0f / static_cast<float>(q.size());
  return c;
}

// cos(marker patch i, frame patch j) for every pair.
//
// Both operands are L2-normalised rows, so this is a cosine and nothing else. The
// operands are NxK and KxN, which is exactly the plain gemm contract, so no
// transposing flag belongs here: passing GEMM_2_T while also handing over an
// already-transposed B makes gemm reinterpret the KxN operand as NxK and demand
// A's width be N, the exception is swallowed by the caller, and the dense path
// silently declines every single frame.
bool cosineMatrix(const cv::Mat& markerPatches, const cv::Mat& framePatches,
                  cv::Mat& sim) {
  const cv::Mat frameT = framePatches.t();
  if (markerPatches.cols != frameT.rows || markerPatches.empty() || framePatches.empty())
    return false;
  try {
    cv::gemm(markerPatches, frameT, 1.0, cv::noArray(), 0.0, sim);
  } catch (const std::exception&) {
    return false;
  }
  if (sim.dims != 2 || sim.rows != markerPatches.rows ||
      sim.cols != framePatches.rows) {
    return false;
  }
  if (sim.type() != CV_32F)
    sim.convertTo(sim, CV_32F);
  return true;
}

// Binomial weights for a window of the given radius, normalised. [1, r, 1] gives the
// 3x3 kernel's centre twice the weight of its edges. Binomial rather than a box so the
// centre of the window is the most trusted part of it -- a marker seen at an angle
// displaces its patches by up to a cell, and a window that leans on its centre
// survives that without smearing.
std::vector<double> binomialKernel(int radius) {
  const int n = 2 * radius + 1;          // window width
  const int total = 2 * radius;          // trials behind C(2r, .)
  std::vector<double> k(static_cast<size_t>(n), 1.0);

  // C(total, 0) = 1 at d = -radius, then each step multiplies by the next binomial
  // ratio. Walking the row multiplicatively rather than filling Pascal's triangle is
  // the difference between a kernel of [1, 2, 1] and a flat [2, 2, 2]: the additive
  // version has to be mirrored to be symmetric, and mirroring the *first* half of an
  // already-increasing run copies the peak's neighbour over the peak.
  double w = 1.0;
  for (int i = 0; i < n; ++i) {
    k[static_cast<size_t>(i)] = w;
    const int step = i + 1;
    if (step <= total)
      w *= static_cast<double>(total - step + 1) / static_cast<double>(step);
  }

  double sum = 0.0;
  for (double v : k)
    sum += v;
  for (double& v : k)
    v /= sum;
  return k;
}

// Separable binomial filter over one axis of a grid-indexed matrix.
//
// `count` cells along that axis, `other` cells on the other axis, `stride` how far one
// cell along this axis moves in memory. Cells outside the grid are dropped and the
// window renormalised, so a patch on the marker's border is not penalised for it.
//
// Spelled out over a flat buffer rather than using a 2D filter, because the two grids
// generally have different shapes and OpenCV's would need them equal.
void filterAxis(const std::vector<float>& in, std::vector<float>& out, int count,
                int other, int stride, const std::vector<double>& kernel) {
  const int radius = (kernel.size() - 1) / 2;
  out.resize(in.size());
  for (int a = 0; a < count; ++a) {
    const size_t base = static_cast<size_t>(a) * stride;
    for (int b = 0; b < other; ++b) {
      double acc = 0.0, weight = 0.0;
      for (int d = -radius; d <= radius; ++d) {
        const int cell = a + d;
        if (cell < 0 || cell >= count)
          continue;
        const double w = kernel[static_cast<size_t>(d + radius)];
        acc += w * in[static_cast<size_t>(cell) * stride + b];
        weight += w;
      }
      out[base + b] = weight > 0.0 ? static_cast<float>(acc / weight) : 0.0f;
    }
  }
}

// The neighbourhood filter. Replaces each similarity by the binomial-weighted mean
// over a (2r+1)^2 window of the two patch grids, which sharpens the smooth ridge a
// real correspondence forms and leaves an isolated spurious peak as a spike.
//
// This is the single biggest lever in the whole dense path: on the supplied clip it
// widens the best/runner-up gap from ~0.01 to a fraction of the full range on good
// frames, and it is a handful of adds over a 196x196 matrix.
bool contextFilter(const cv::Mat& sim, cv::Size markerGrid, cv::Size frameGrid,
                   int radius, cv::Mat& out) {
  if (radius <= 0) {
    out = sim;
    return true;
  }
  if (markerGrid.width <= 0 || markerGrid.height <= 0 || frameGrid.width <= 0 ||
      frameGrid.height <= 0) {
    return false;
  }
  const int markerCells = markerGrid.width * markerGrid.height;
  const int frameCells = frameGrid.width * frameGrid.height;
  if (markerCells != sim.rows || frameCells != sim.cols)
    return false;

  const std::vector<double> kernel = binomialKernel(radius);
  std::vector<float> a(static_cast<size_t>(sim.rows) * sim.cols), b;
  std::memcpy(a.data(), sim.ptr<float>(), a.size() * sizeof(float));

  // Marker axis, then frame axis.
  filterAxis(a, b, markerGrid.height, markerGrid.width * frameCells, markerGrid.width,
             kernel);
  filterAxis(b, a, frameGrid.height, markerCells, 1, kernel);

  out.create(sim.rows, sim.cols, CV_32F);
  std::memcpy(out.ptr<float>(), a.data(), a.size() * sizeof(float));
  return true;
}

// Least-squares affine fit: u = a.x + b.y + c, v = d.x + e.y + f.
//
// Two independent 3-unknown normal equations, but solved in centred coordinates. On
// raw pixel coordinates the constant term competes with two large linear terms in one
// 3x3 solve, and the condition number here is exactly the sort of thing that
// silently costs a digit of accuracy. Centring on the centroid makes the coordinates
// independent and the solve well posed; the fit it describes is the same fit.
//
// The centred design has one fewer unknown per channel -- there is no intercept left,
// because a centred coordinate has zero mean -- so each channel is a 2x2 solve against
// the Gram matrix [[sxx, sxy], [sxy, syy]], and the intercept is recovered afterwards
// as the mean of the targets.
bool fitAffineLeastSquares(const std::vector<cv::Point2f>& src,
                           const std::vector<cv::Point2f>& dst,
                           const std::vector<int>& use, cv::Mat& out) {
  const size_t n = use.size();
  if (n < 3)
    return false;

  double mx = 0.0, my = 0.0;
  for (int i : use) {
    mx += src[static_cast<size_t>(i)].x;
    my += src[static_cast<size_t>(i)].y;
  }
  mx /= static_cast<double>(n);
  my /= static_cast<double>(n);

  double sxx = 0.0, sxy = 0.0, syy = 0.0, mu = 0.0, mv = 0.0;
  for (int i : use) {
    const double x = src[static_cast<size_t>(i)].x - mx;
    const double y = src[static_cast<size_t>(i)].y - my;
    sxx += x * x;
    sxy += x * y;
    syy += y * y;
    mu += dst[static_cast<size_t>(i)].x;
    mv += dst[static_cast<size_t>(i)].y;
  }
  mu /= static_cast<double>(n);
  mv /= static_cast<double>(n);

  const double det = sxx * syy - sxy * sxy;
  if (std::abs(det) < 1e-9)
    return false;  // collinear; an affine map is not determined

  double rux = 0.0, ruy = 0.0, rvx = 0.0, rvy = 0.0;
  for (int i : use) {
    const double x = src[static_cast<size_t>(i)].x - mx;
    const double y = src[static_cast<size_t>(i)].y - my;
    const double du = dst[static_cast<size_t>(i)].x - mu;
    const double dv = dst[static_cast<size_t>(i)].y - mv;
    rux += x * du;
    ruy += y * du;
    rvx += x * dv;
    rvy += y * dv;
  }

  const double a = (syy * rux - sxy * ruy) / det;
  const double b = (sxx * ruy - sxy * rux) / det;
  const double d = (syy * rvx - sxy * rvy) / det;
  const double e = (sxx * rvy - sxy * rvx) / det;
  if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(d) || !std::isfinite(e))
    return false;

  out = cv::Mat::zeros(3, 3, CV_64F);
  out.at<double>(0, 0) = a;
  out.at<double>(0, 1) = b;
  out.at<double>(0, 2) = mu - a * mx - b * my;
  out.at<double>(1, 0) = d;
  out.at<double>(1, 1) = e;
  out.at<double>(1, 2) = mv - d * mx - e * my;
  out.at<double>(2, 2) = 1.0;
  return true;
}

// How many correspondences a transform explains, at `threshold`, into `support`.
int countReprojectionInliers(const cv::Mat& model, const std::vector<cv::Point2f>& src,
                             const std::vector<cv::Point2f>& dst, double threshold,
                             std::vector<int>* support) {
  if (support)
    support->clear();
  if (model.empty() || src.size() != dst.size())
    return 0;
  const double thresholdSq = threshold * threshold;
  std::vector<cv::Point2f> projected;
  try {
    cv::perspectiveTransform(src, projected, model);
  } catch (const std::exception&) {
    return 0;
  }
  int inliers = 0;
  for (size_t i = 0; i < src.size(); ++i) {
    const double dx = projected[i].x - dst[i].x;
    const double dy = projected[i].y - dst[i].y;
    if (std::isfinite(dx) && std::isfinite(dy) && dx * dx + dy * dy <= thresholdSq) {
      ++inliers;
      if (support)
        support->push_back(static_cast<int>(i));
    }
  }
  return inliers;
}

// The affine part of a 3x3 homography: its first two rows over a normalised bottom
// row. This is the 6-DOF model, obtained from an 8-DOF fit by discarding exactly the
// term that runs away.
cv::Mat affinePart(const cv::Mat& h) {
  cv::Mat affine = cv::Mat::eye(3, 3, CV_64F);
  if (h.rows == 3 && h.cols == 3) {
    h.row(0).copyTo(affine.row(0));
    h.row(1).copyTo(affine.row(1));
  }
  return affine;
}

// A patch centre, in the crop's own pixels. The outermost centres sit half a cell
// inside the crop, which is why the outline is the crop's *corners* put through the
// fit rather than the grid's extremes.
cv::Point2f patchCentre(int index, cv::Size grid, cv::Size2f crop) {
  const int cols = std::max(1, grid.width);
  const int c = index % cols;
  const int r = index / cols;
  return cv::Point2f((static_cast<float>(c) + 0.5f) * crop.width / cols,
                     (static_cast<float>(r) + 0.5f) * crop.height /
                         std::max(1, grid.height));
}

// Geometry of a candidate outline, measured before any judgement is made on it.
//
// Every quantity is a fraction of the frame or a sign, never an absolute pixel count. A
// homography fitted to patch centres can satisfy an inlier test and still be nonsense
// -- collapsed to a line, folded into a bowtie, shrunk to a speck, or sent off to
// infinity -- and which of those happened is the whole diagnosis.
struct QuadShape {
  double area = 0.0;
  double areaFraction = 0.0;  // of the frame
  float minSpan = 0.0f;       // shortest corner-to-corner distance, px
  bool finite = false;
  bool convex = false;
};

QuadShape measureQuad(const std::vector<cv::Point2f>& corners, cv::Size frameSize) {
  QuadShape shape;
  if (corners.size() != 4)
    return shape;

  shape.finite = true;
  for (const auto& c : corners)
    shape.finite &= std::isfinite(c.x) && std::isfinite(c.y);
  if (!shape.finite)
    return shape;

  shape.area = std::abs(signedArea(corners));
  const double frameArea = std::max(1.0, static_cast<double>(frameSize.width) * frameSize.height);
  shape.areaFraction = shape.area / frameArea;

  shape.minSpan = std::numeric_limits<float>::max();
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j)
      shape.minSpan = std::min(shape.minSpan, static_cast<float>(cv::norm(corners[i] - corners[j])));

  // Convexity: for a simple quad in order, all four turns have the same sign. A
  // bowtie -- the outline folded over itself, which is what a projective fit does when
  // its denominator changes sign inside the region -- produces two of each.
  int turns = 0;
  for (int i = 0; i < 4; ++i) {
    const cv::Point2f& a = corners[i];
    const cv::Point2f& b = corners[(i + 1) % 4];
    const cv::Point2f& c = corners[(i + 2) % 4];
    const double cross = static_cast<double>(b.x - a.x) * (c.y - b.y) -
                         static_cast<double>(b.y - a.y) * (c.x - b.x);
    if (std::abs(cross) < 1e-9)
      return shape;  // a straight or doubled edge: not a quad
    if (cross > 0.0)
      ++turns;
  }
  shape.convex = (turns == 4 || turns == 0);
  return shape;
}

}  // namespace

void GeometricVerifier::setConfig(const Config& cfg) {
  cfg_ = cfg;
  // A non-maximum radius below 1 would mean "the runner-up may be a neighbour of the
  // winner", which is the defect the disc exists to remove. Clamp it so no combination
  // of the knobs can quietly reintroduce it.
  cfg_.nmsRadius = std::max(1, cfg_.nmsRadius);
  cfg_.contextRadius = std::max(0, cfg_.contextRadius);
  cfg_.voteHypotheses = std::max(0, cfg_.voteHypotheses);
  cfg_.minMatches = std::max(4, cfg_.minMatches);
  cfg_.minInliers = std::max(4, cfg_.minInliers);
  cfg_.voteThresholdCells = std::max(0.25f, cfg_.voteThresholdCells);
  cfg_.refineThresholdCells = std::max(0.05f, cfg_.refineThresholdCells);
  // The tuning constants here are the ones the temporal filter is defined in terms
  // of, so a changed limit invalidates the pose it is limiting.
  reset();
}

void GeometricVerifier::reset() {
  trackedId_ = -1;
  frame_ = cv::Size();
  seeded_ = false;
  held_ = 0;
  pose_ = Pose{};
}

bool GeometricVerifier::fitAndScore(const std::vector<cv::Point2f>& src,
                                    const std::vector<cv::Point2f>& dst, int method,
                                    double threshold, cv::Mat& out, int& inliers,
                                    std::vector<int>* support) const {
  out.release();
  inliers = 0;
  if (support)
    support->clear();
  if (src.size() < 4)
    return false;

  cv::Mat mask, model;
  try {
    model = cv::findHomography(src, dst, method, threshold, mask, cfg_.maxIterations,
                               cfg_.confidence);
  } catch (const std::exception&) {
    try {
      model = cv::findHomography(src, dst, cv::RANSAC, threshold, mask,
                                 cfg_.maxIterations, cfg_.confidence);
    } catch (const std::exception&) {
      return false;
    }
  }
  if (model.empty() || model.rows != 3 || model.cols != 3)
    return false;

  if (support && !mask.empty() && mask.type() == CV_8U && mask.total() == src.size()) {
    for (size_t i = 0; i < mask.total(); ++i)
      if (mask.at<unsigned char>(static_cast<int>(i)) != 0)
        support->push_back(static_cast<int>(i));
    inliers = static_cast<int>(support->size());
  } else {
    inliers = (!mask.empty() && mask.type() == CV_8U)
                  ? cv::countNonZero(mask)
                  : static_cast<int>(src.size());
  }
  out = model;
  return true;
}

bool GeometricVerifier::fitAffineRobust(const std::vector<cv::Point2f>& src,
                                        const std::vector<cv::Point2f>& dst,
                                        double threshold, const std::vector<int>* seed,
                                        cv::Mat& out, int& inliers) const {
  std::vector<int> use;
  if (seed && seed->size() >= 3)
    use = *seed;
  else
    for (size_t i = 0; i < src.size(); ++i)
      use.push_back(static_cast<int>(i));

  out.release();
  inliers = 0;
  for (int round = 0; round < 3; ++round) {
    cv::Mat candidate;
    if (!fitAffineLeastSquares(src, dst, use, candidate))
      return false;
    std::vector<int> support;
    const int count = countReprojectionInliers(candidate, src, dst, threshold, &support);
    if (count < 3)
      return false;
    if (support == use) {  // the inlier set stopped moving
      out = candidate;
      inliers = count;
      return true;
    }
    if (count >= inliers) {
      out = candidate;
      inliers = count;
    }
    if (support.size() < use.size())
      use = support;  // never grow: only a tighter consensus is a better seed
  }
  return !out.empty();
}

bool GeometricVerifier::buildCandidates(const MarkerRecord& marker, cv::Size queryGrid,
                                        const cv::Rect& roi,
                                        const std::vector<int>& bestIndex,
                                        const std::vector<float>& bestCtx,
                                        const std::vector<int>& secondIndex,
                                        const std::vector<float>& secondCtx,
                                        const std::vector<int>& bestOfFrame,
                                        Candidates& out, Verification& diag) const {
  const int count = static_cast<int>(bestIndex.size());
  const cv::Size2f frameCrop(static_cast<float>(roi.width), static_cast<float>(roi.height));
  const cv::Size markerGrid = marker.grid.shape;

  diag.patches = count;
  for (int i = 0; i < count; ++i) {
    const int j = bestIndex[i];
    if (j < 0 || bestCtx[i] < cfg_.patchFloor)
      continue;
    ++diag.aboveFloor;

    if (cfg_.useRatioTest) {
      const int k = secondIndex[i];
      if (k < 0)
        continue;
      const float bestDistance = 1.0f - bestCtx[i];
      const float secondDistance = 1.0f - secondCtx[i];
      if (secondDistance <= 0.0f)
        continue;
      if (bestDistance >= cfg_.patchGap * secondDistance)
        continue;
    }
    if (cfg_.useMutualCheck && bestOfFrame[static_cast<size_t>(j)] != i)
      continue;

    // The candidate's frame position is a patch centre in the query crop, so it needs
    // the ROI's origin put back before it is in frame coordinates.
    const cv::Point2f local = patchCentre(j, queryGrid, frameCrop);
    const cv::Point2f inRoi(local.x + static_cast<float>(roi.x),
                            local.y + static_cast<float>(roi.y));
    out.markerPoints.push_back(
        patchCentre(i, markerGrid, marker.grid.area));
    out.framePoints.push_back(inRoi);
    out.candidates.push_back(i);
  }
  return !out.candidates.empty();
}

bool GeometricVerifier::vote(const Candidates& candidates, const GridSpace& space,
                              cv::Mat& h, std::vector<unsigned char>& support,
                              Verification& diag) const {
  const int n = static_cast<int>(candidates.candidates.size());
  if (n < cfg_.minMatches)
    return false;

  // Everything here is measured in the *marker's* crop pixels, because that is the one
  // space the hypotheses and the candidates already share -- a marker cell and a frame
  // cell are different sizes whenever the marker covers a different fraction of the
  // two crops, and converting between them per hypothesis is needless.
  const double tolerance = std::max(1e-6, space.cell) * cfg_.voteThresholdCells;
  const double toleranceSq = tolerance * tolerance;

  std::vector<cv::Point2f> projected;
  std::vector<unsigned char> mask(static_cast<size_t>(n), 0);

  auto score = [&](const cv::Mat& candidate) -> int {
    if (candidate.empty() || candidate.rows != 3 || candidate.cols != 3)
      return -1;
    // A vanishing third row maps everything onto a line: a degenerate answer rather
    // than a wrong one, so it is skipped rather than scored.
    if (std::abs(candidate.at<double>(2, 2)) < 1e-12)
      return -1;
    try {
      cv::perspectiveTransform(candidates.markerPoints, projected, candidate);
    } catch (const std::exception&) {
      return -1;
    }
    int count = 0;
    for (int i = 0; i < n; ++i) {
      const double dx = projected[i].x - candidates.framePoints[i].x;
      const double dy = projected[i].y - candidates.framePoints[i].y;
      if (!std::isfinite(dx) || !std::isfinite(dy))
        return -1;
      if (dx * dx + dy * dy <= toleranceSq) {
        mask[static_cast<size_t>(i)] = 1;
        ++count;
      } else {
        mask[static_cast<size_t>(i)] = 0;
      }
    }
    return count;
  };

  // Deterministic hypothesis generation, and the seed geometry is the whole thing.
  //
  // Four seed points have to span the marker. Four *adjacent* ones are nearly
  // collinear, which gives a transform with an almost unconstrained denominator --
  // one that maps most of the grid into tolerance while being nonsense, so it wins the
  // vote on support count and produces a wild outline.
  //
  // Striding the candidate list does not work at all, for a second reason: the list is
  // row-major, so a stride that is not a multiple of the grid width walks the seeds
  // across the rows in a zig-zag. Four points picked that way form a
  // self-intersecting quadrilateral whose shoelace area is identically zero.
  //
  // So the seeds are chosen in grid coordinates instead: the candidates extremal in a
  // rotated frame, u = r*cos(t) + c*sin(t) and v = -r*sin(t) + c*cos(t). At t = 0 that
  // is exactly the marker's four corners; sweeping t over a quarter turn gives
  // genuinely different quads, all spanning the marker's extent, all simple.
  //
  // A quarter turn is the period of the rotation -- beyond it the four corners repeat
  // -- so the family is (rotations x insets), and insets matter as much as rotations:
  // on a 14x14 grid the extremal four points only change when the rotation passes a
  // tie, so most of a rotation-only sweep selects the *same* quad, which corrupts the
  // margin because the runner-up then has the winner's score by construction.
  int bestCount = -1, runnerUpCount = -1, supportedCount = 0;
  std::vector<unsigned char> bestMask;
  cv::Mat best;

  const int gridWidth = std::max(1, space.grid.width);
  std::vector<cv::Point2f> gridPos(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const int patch = candidates.candidates[static_cast<size_t>(i)];
    gridPos[static_cast<size_t>(i)] =
        cv::Point2f(static_cast<float>(patch % gridWidth),
                    static_cast<float>(patch / gridWidth));
  }

  const int rotations = std::max(1, cfg_.voteHypotheses / 4);
  static const double kInsets[4] = {0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0};
  int tried = 0;

  for (int ri = 0; ri < rotations; ++ri) {
    const double theta = kHalfPi * ri / rotations;
    const double ct = std::cos(theta), st = std::sin(theta);

    double uLo = 0, uHi = 0, vLo = 0, vHi = 0;
    for (int i = 0; i < n; ++i) {
      const double u = gridPos[static_cast<size_t>(i)].x * ct + gridPos[static_cast<size_t>(i)].y * st;
      const double v = -gridPos[static_cast<size_t>(i)].x * st + gridPos[static_cast<size_t>(i)].y * ct;
      if (i == 0 || u < uLo) uLo = u;
      if (i == 0 || u > uHi) uHi = u;
      if (i == 0 || v < vLo) vLo = v;
      if (i == 0 || v > vHi) vHi = v;
    }

    for (double inset : kInsets) {
      const double u0 = uLo + inset * (uHi - uLo), u1 = uHi - inset * (uHi - uLo);
      const double v0 = vLo + inset * (vHi - vLo), v1 = vHi - inset * (vHi - vLo);

      int seed[4];
      bool degenerate = false;
      for (int corner = 0; corner < 4; ++corner) {
        const double tu = (corner == 0 || corner == 3) ? u0 : u1;
        const double tv = (corner < 2) ? v0 : v1;
        int pick = -1;
        double bestD = 0;
        for (int i = 0; i < n; ++i) {
          const double u = gridPos[static_cast<size_t>(i)].x * ct + gridPos[static_cast<size_t>(i)].y * st;
          const double v = -gridPos[static_cast<size_t>(i)].x * st + gridPos[static_cast<size_t>(i)].y * ct;
          const double d = (u - tu) * (u - tu) + (v - tv) * (v - tv);
          if (pick < 0 || d < bestD) {
            bestD = d;
            pick = i;
          }
        }
        // Two corners of a thin candidate set can land on the same patch, and a
        // repeated point makes the fit meaningless rather than merely bad. No
        // deduplication sort here on purpose: the four indices are already in cyclic
        // order, and sorting them by marker-patch index -- row-major over the grid --
        // reorders them into (r0c0, r0c1, r1c0, r1c1), which is a bowtie whose shoelace
        // area is zero.
        for (int other = 0; other < corner; ++other)
          if (seed[other] == pick)
            degenerate = true;
        seed[corner] = pick;
      }
      if (degenerate)
        continue;

      std::vector<cv::Point2f> src, dst;
      src.reserve(4);
      dst.reserve(4);
      for (int k = 0; k < 4; ++k) {
        src.push_back(candidates.markerPoints[static_cast<size_t>(seed[k])]);
        dst.push_back(candidates.framePoints[static_cast<size_t>(seed[k])]);
      }
      // The area test, not a try/catch: four points from a 14x14 grid are almost never
      // degenerate, and findHomography throws rather than returning an empty matrix
      // for the ones that are.
      if (std::abs(signedArea(src)) < 1.0)
        continue;
      ++tried;

      cv::Mat candidate;
      try {
        candidate = cv::findHomography(src, dst, 0);
      } catch (const std::exception&) {
        continue;
      }
      const int count = score(candidate);
      if (count < 0)
        continue;
      if (count >= cfg_.minInliers)
        ++supportedCount;
      if (count > bestCount) {
        runnerUpCount = bestCount;
        bestCount = count;
        best = candidate;
        bestMask = mask;
      } else if (count > runnerUpCount) {
        runnerUpCount = count;
      }
    }
  }

  diag.votesTried = tried;
  diag.votesSupported = supportedCount;
  diag.voteBest = std::max(0, bestCount);
  diag.voteRunnerUp = std::max(0, runnerUpCount);

  if (bestCount < cfg_.minInliers || best.empty())
    return false;

  h = best;
  support = std::move(bestMask);
  return true;
}

bool GeometricVerifier::project(const MarkerRecord& marker, const cv::Mat& h,
                                const cv::Rect& roi, const cv::Size& frameSize,
                                Verification& out, Verification& diag) const {
  // The crop corners, in the marker's own crop pixels, are the only points outside
  // the patch grid: the grid measures patch *centres*, so its outermost centres sit
  // half a cell inside the crop. Sending the four corners through the fitted transform
  // is what turns a set of correspondences into an outline.
  const std::vector<cv::Point2f> crop = {
      cv::Point2f(0.0f, 0.0f),
      cv::Point2f(marker.grid.area.width, 0.0f),
      cv::Point2f(marker.grid.area.width, marker.grid.area.height),
      cv::Point2f(0.0f, marker.grid.area.height)};

  auto place = [&](std::vector<cv::Point2f>& corners) {
    for (auto& c : corners)
      c += cv::Point2f(static_cast<float>(roi.x), static_cast<float>(roi.y));
  };

  std::vector<cv::Point2f> corners;
  try {
    cv::perspectiveTransform(crop, corners, h);
  } catch (const std::exception&) {
    diag.reject = "the fitted transform cannot be projected";
    return false;
  }
  place(corners);
  QuadShape shape = measureQuad(corners, frameSize);

  // The projective part is the least constrained thing about the fit: it is determined
  // by how the patch grid recedes, and a grid whose patches are tens of pixels apart
  // barely constrains it at all. When the denominator changes sign inside the marker's
  // own extent the corners leave the frame or go non-finite while the patch centres it
  // was fitted to still agree -- an outline that is a consequence of a
  // well-conditioned fit being extrapolated past where it is known.
  //
  // The affine part of the same transform, its first two rows, cannot do that: it has
  // no denominator to change sign. It is the correct answer whenever the true view is
  // only weakly projective, which is most views of a marker this close to the camera.
  if (!shape.finite || !shape.convex) {
    std::vector<cv::Point2f> affineCorners;
    try {
      cv::perspectiveTransform(crop, affineCorners, affinePart(h));
    } catch (const std::exception&) {
      diag.reject = "the fitted transform cannot be projected";
      return false;
    }
    place(affineCorners);
    const QuadShape affineShape = measureQuad(affineCorners, frameSize);
    if (affineShape.finite) {
      corners = std::move(affineCorners);
      shape = affineShape;
      diag.affineFallback = true;
    }
  }

  diag.areaFraction = shape.areaFraction;
  diag.minSpan = shape.minSpan;

  if (!shape.finite) {
    diag.reject = "the fitted transform sends the outline off to infinity";
    return false;
  }

  // The bounds are physical rather than tuned: no part of a marker occupies more than
  // the frame it was found in, and a marker is never a speck.
  const double frameWidth = std::max(1, frameSize.width);
  const bool bigEnough = shape.areaFraction >= cfg_.minQuadAreaFraction &&
                         shape.areaFraction <= cfg_.maxQuadAreaFraction &&
                         shape.minSpan >= cfg_.minQuadSpanFraction * frameWidth;
  if (!bigEnough) {
    diag.reject = "the fitted marker is degenerate";
    return false;
  }
  if (cfg_.requireConvexQuad && !shape.convex) {
    diag.reject = "the outline folds over itself";
    return false;
  }

  out.corners = std::move(corners);
  out.ok = true;
  return true;
}

Verification GeometricVerifier::dense(const MarkerRecord& marker, const PatchGrid& query,
                                     const cv::Rect& roi, const cv::Size& frameSize,
                                     Verification& diag) {
  Verification out;
  out.which = Localiser::Dense;

  if (marker.grid.empty() || query.empty()) {
    out.decline = Decline::NoReference;
    diag.decline = Decline::NoReference;
    diag.reject = out.reject = "no patch grid";
    return out;
  }
  if (marker.grid.descriptors.cols != query.descriptors.cols) {
    out.decline = Decline::NoReference;
    diag.decline = Decline::NoReference;
    diag.reject = out.reject = "patch dimensions differ";
    return out;
  }
  if (roi.width <= 0 || roi.height <= 0 || frameSize.width <= 0 || frameSize.height <= 0) {
    out.decline = Decline::BadGeometry;
    diag.decline = Decline::BadGeometry;
    diag.reject = out.reject = "empty region";
    return out;
  }

  cv::Mat sim;
  if (!cosineMatrix(marker.grid.descriptors, query.descriptors, sim)) {
    out.decline = Decline::NoSimilarity;
    diag.decline = Decline::NoSimilarity;
    diag.reject = out.reject = "the patch grids cannot be compared";
    return out;
  }

  cv::Mat context;
  if (!contextFilter(sim, marker.grid.shape, query.shape, cfg_.contextRadius, context)) {
    out.decline = Decline::NoSimilarity;
    diag.decline = Decline::NoSimilarity;
    diag.reject = out.reject = "the patch grids are not rectangular";
    return out;
  }

  // Best, and the best outside a disc of `nmsRadius` cells around it, plus the reverse
  // lookup a mutual check needs. Keeping the reverse map is what stops a patch that is
  // the runner-up everywhere from being matched repeatedly.
  const int patches = marker.grid.count();
  const int framePatches = query.count();
  const int frameCols = std::max(1, query.shape.width);
  const int nms = cfg_.nmsRadius;

  std::vector<int> bestIndex(static_cast<size_t>(patches));
  std::vector<int> secondIndex(static_cast<size_t>(patches));
  std::vector<float> bestCtx(static_cast<size_t>(patches));
  std::vector<float> secondCtx(static_cast<size_t>(patches));
  std::vector<int> bestOfFrame(static_cast<size_t>(framePatches), -1);
  std::vector<float> bestOfFrameCtx(static_cast<size_t>(framePatches), -2.0f);

  for (int i = 0; i < patches; ++i) {
    const float* row = context.ptr<float>(i);
    int best = -1, second = -1;
    float bestScore = -2.0f, secondScore = -2.0f;
    for (int j = 0; j < framePatches; ++j) {
      const float s = row[j];
      if (s > bestScore) {
        bestScore = s;
        best = j;
      }
    }
    if (best >= 0) {
      const int br = best / frameCols, bc = best % frameCols;
      for (int j = 0; j < framePatches; ++j) {
        const int r = j / frameCols, c = j % frameCols;
        if (std::abs(r - br) <= nms && std::abs(c - bc) <= nms)
          continue;  // inside the disc: not a competing hypothesis
        const float s = row[j];
        if (s > secondScore) {
          secondScore = s;
          second = j;
        }
      }
    }
    bestIndex[static_cast<size_t>(i)] = best;
    secondIndex[static_cast<size_t>(i)] = second;
    bestCtx[static_cast<size_t>(i)] = best >= 0 ? bestScore : -1.0f;
    secondCtx[static_cast<size_t>(i)] = second >= 0 ? secondScore : -1.0f;
    if (best >= 0 && bestScore > bestOfFrameCtx[static_cast<size_t>(best)]) {
      bestOfFrameCtx[static_cast<size_t>(best)] = bestScore;
      bestOfFrame[static_cast<size_t>(best)] = i;
    }
  }

  // The query grid is needed again to place candidates in frame coordinates.
  Candidates candidates;
  if (!buildCandidates(marker, query.shape, roi, bestIndex, bestCtx, secondIndex,
                       secondCtx, bestOfFrame, candidates, diag)) {
    out.decline = Decline::TooFewMatches;
    diag.decline = Decline::TooFewMatches;
    diag.reject = out.reject = "only " + std::to_string(diag.aboveFloor) +
                                " of " + std::to_string(patches) + " patches cleared the floor";
    return out;
  }

  diag.correspondences = static_cast<int>(candidates.candidates.size());
  if (diag.correspondences < cfg_.minMatches) {
    out.decline = Decline::TooFewMatches;
    diag.decline = Decline::TooFewMatches;
    diag.reject = out.reject = "only " + std::to_string(diag.correspondences) +
                                " correspondences for " + std::to_string(cfg_.minMatches) +
                                " needed";
    return out;
  }

  GridSpace space;
  space.grid = marker.grid.shape;
  space.size = marker.grid.area;
  space.cell = std::min(
      space.grid.width > 0 ? space.size.width / space.grid.width : 1.0f,
      space.grid.height > 0 ? space.size.height / space.grid.height : 1.0f);

  std::vector<unsigned char> support(static_cast<size_t>(diag.correspondences), 1);
  cv::Mat h;
  if (cfg_.voteHypotheses > 0) {
    if (!vote(candidates, space, h, support, diag)) {
      out.decline = Decline::NoVote;
      diag.decline = Decline::NoVote;
      diag.reject = out.reject = "no hypothesis of " + std::to_string(diag.votesTried) +
                                  " reached " + std::to_string(cfg_.minInliers) +
                                  " agreeing patches";
      return out;
    }
  } else {
    // No voting: the loose set is the support set and the estimator finds the
    // transform directly. Kept so the sweep can measure what the vote buys.
    try {
      h = cv::findHomography(candidates.markerPoints, candidates.framePoints, 0);
    } catch (const std::exception&) {
      out.decline = Decline::FitFailed;
      diag.decline = Decline::FitFailed;
      diag.reject = out.reject = "no homography could be fitted";
      return out;
    }
  }

  // Only the correspondences the winner already supports are handed to the estimator.
  // That is the point of voting: the estimator gets a set it does not have to
  // re-decide, so its budget goes into refining the transform rather than into
  // rediscovering which correspondences are real.
  std::vector<cv::Point2f> src, dst;
  src.reserve(support.size());
  dst.reserve(support.size());
  for (size_t i = 0; i < support.size(); ++i) {
    if (!support[i])
      continue;
    src.push_back(candidates.markerPoints[i]);
    dst.push_back(candidates.framePoints[i]);
  }
  const int supported = static_cast<int>(src.size());
  if (supported < 4) {
    out.decline = Decline::TooFewSupported;
    diag.decline = Decline::TooFewSupported;
    diag.reject = out.reject = "only " + std::to_string(supported) + " agreed with the winner";
    return out;
  }

  // A fraction of one patch cell of the marker's crop, floored at two pixels so that a
  // very small crop cannot turn the tolerance into a sub-pixel test nothing survives.
  const double threshold =
      std::max(2.0, std::max(1e-6, space.cell) * cfg_.refineThresholdCells);

  cv::Mat homography, affine;
  std::vector<int> seed;
  const bool haveHomography = cfg_.geometry != Config::Geometry::Affine &&
                              fitAndScore(src, dst, cv::USAC_MAGSAC, threshold, homography,
                                          diag.homographyInliers, &seed);

  // The affine fit is seeded with the homography's inliers when there is one. That seed
  // is what makes it robust: it inherits an already-consensus set instead of running
  // its own search over a field that still contains every spurious correspondence the
  // vote let through.
  const bool haveAffine = cfg_.geometry != Config::Geometry::Homography &&
                          fitAffineRobust(src, dst, threshold,
                                          haveHomography ? &seed : nullptr, affine,
                                          diag.affineInliers);

  // Model selection. The 8-DOF fit is only preferred when it explains materially more
  // of the support set than the 6-DOF one; otherwise the affine model wins, because the
  // two agree about where the marker is and only one of them is capable of producing an
  // outline no camera could have seen. Preferring the model with more freedom is the
  // wrong default when the extra freedom is fitted to noise.
  cv::Mat chosen;
  int chosenInliers = 0;
  if (haveHomography && haveAffine) {
    const int margin = static_cast<int>(std::ceil(cfg_.homographyMargin * supported));
    if (diag.homographyInliers >= diag.affineInliers + margin) {
      chosen = homography;
      chosenInliers = diag.homographyInliers;
      diag.modelHomography = true;
    } else {
      chosen = affine;
      chosenInliers = diag.affineInliers;
    }
  } else if (haveHomography) {
    chosen = homography;
    chosenInliers = diag.homographyInliers;
    diag.modelHomography = true;
  } else if (haveAffine) {
    chosen = affine;
    chosenInliers = diag.affineInliers;
  } else {
    out.decline = Decline::FitFailed;
    diag.decline = Decline::FitFailed;
    diag.reject = out.reject = "no transform could be fitted";
    return out;
  }

  out.inliers = chosenInliers;
  out.inlierRatio = static_cast<float>(chosenInliers) / std::max(1, supported);
  if (chosenInliers < cfg_.minInliers || out.inlierRatio < cfg_.minInlierRatio) {
    // Correspondences existed but they did not agree on one marker.
    out.decline = Decline::InlierGate;
    diag.decline = Decline::InlierGate;
    diag.reject = out.reject = std::to_string(chosenInliers) + " of " +
                                std::to_string(supported) + " agreed (" +
                                std::to_string(static_cast<int>(out.inlierRatio * 100.0f)) +
                                "%)";
    return out;
  }

  if (!project(marker, chosen, roi, frameSize, out, diag)) {
    out.decline = Decline::DegenerateQuad;
    diag.decline = Decline::DegenerateQuad;
    out.reject = diag.reject;
    return out;
  }
  return out;
}

Verification GeometricVerifier::keypoints(const MarkerRecord& marker,
                                         const cv::UMat& frame, const cv::Rect& frameRect,
                                         const cv::Size& frameSize) {
  Verification out;
  out.which = Localiser::Keypoints;

  if (frame.empty() || marker.keypoints.empty() || marker.descriptors.empty()) {
    out.reject = "no keypoints";
    return out;
  }
  if (frameRect.width <= 0 || frameRect.height <= 0 || frameSize.width <= 0 ||
      frameSize.height <= 0) {
    out.reject = "empty frame";
    return out;
  }

  try {
    cv::UMat grayUmat;
    if (frame.channels() == 1) {
      grayUmat = frame;
    } else if (frame.channels() == 4) {
      cv::cvtColor(frame, grayUmat, cv::COLOR_BGRA2GRAY);
    } else {
      cv::cvtColor(frame, grayUmat, cv::COLOR_BGR2GRAY);
    }
    const cv::Mat gray = grayUmat.getMat(cv::ACCESS_READ);

    std::vector<cv::KeyPoint> frameKeypoints;
    cv::Mat frameDescriptors;
    detector_->detectAndCompute(gray, cv::noArray(), frameKeypoints, frameDescriptors);
    if (frameKeypoints.size() < 4 || frameDescriptors.empty() ||
        frameDescriptors.type() != CV_8U || marker.descriptors.type() != CV_8U) {
      out.reject = "nothing detected in the frame";
      return out;
    }

    // Ratio test, on the marker's keypoints querying the frame. This is the
    // Config::ratioThreshold the previous version declared and never used.
    cv::Ptr<cv::BFMatcher> matcher = cv::BFMatcher::create(cv::NORM_HAMMING, false);
    std::vector<std::vector<cv::DMatch>> knn;
    try {
      matcher->knnMatch(marker.descriptors, frameDescriptors, knn, 2);
    } catch (const std::exception&) {
      out.reject = "matching failed";
      return out;
    }

    std::vector<cv::DMatch> good;
    good.reserve(knn.size());
    for (const auto& pair : knn) {
      if (pair.size() < 2)
        continue;
      const cv::DMatch& first = pair[0];
      if (first.distance <= 0.0f)
        continue;
      // 255 is the largest possible Hamming distance, so a distance that is a large
      // fraction of it carries no information at all.
      if (first.distance > 0.6f * 255.0f)
        continue;
      if (first.distance >= cfg_.keypointRatio * pair[1].distance)
        continue;
      good.push_back(first);
    }
    if (good.size() < 4) {
      out.reject = "only " + std::to_string(good.size()) + " ratio-test matches";
      return out;
    }
    if (good.size() > cfg_.keypointMaxMatches) {
      std::partial_sort(good.begin(), good.begin() + cfg_.keypointMaxMatches, good.end(),
                        [](const cv::DMatch& a, const cv::DMatch& b) {
                          return a.distance < b.distance;
                        });
      good.resize(cfg_.keypointMaxMatches);
    }

    const float markerW = static_cast<float>(std::max(1, marker.thumbnail.cols));
    const float markerH = static_cast<float>(std::max(1, marker.thumbnail.rows));
    std::vector<cv::Point2f> src, dst;
    src.reserve(good.size());
    dst.reserve(good.size());
    for (const cv::DMatch& m : good) {
      if (m.queryIdx >= static_cast<int>(marker.keypoints.size()) ||
          m.trainIdx >= static_cast<int>(frameKeypoints.size()))
        continue;
      // Both sides stay in pixels. Normalising the source to the unit square to match
      // the frame's scale makes the fit's design matrix two orders of magnitude smaller
      // in one direction than the other, which is exactly the conditioning that makes
      // a weak-projective-term fit wander.
      src.push_back(marker.keypoints[static_cast<size_t>(m.queryIdx)].pt);
      dst.push_back(frameKeypoints[static_cast<size_t>(m.trainIdx)].pt +
                    cv::Point2f(static_cast<float>(frameRect.x),
                                static_cast<float>(frameRect.y)));
    }
    if (src.size() < 4) {
      out.reject = "too few usable matches";
      return out;
    }

    const std::vector<cv::Point2f> corners = {cv::Point2f(0.f, 0.f),
                                              cv::Point2f(markerW, 0.f),
                                              cv::Point2f(markerW, markerH),
                                              cv::Point2f(0.f, markerH)};
    cv::Mat model;
    int inliers = 0;
    if (!fitAndScore(src, dst, cv::USAC_MAGSAC, cfg_.keypointReprojection, model,
                     inliers)) {
      out.reject = "no transform could be fitted";
      return out;
    }
    try {
      cv::perspectiveTransform(corners, out.corners, model);
    } catch (const std::exception&) {
      out.reject = "projection failed";
      return out;
    }
    const QuadShape shape = measureQuad(out.corners, frameSize);
    if (!shape.finite || shape.convex == false ||
        shape.areaFraction > cfg_.maxQuadAreaFraction ||
        shape.minSpan < cfg_.minQuadSpanFraction * frameSize.width) {
      out.reject = "the fitted marker is degenerate";
      out.corners.clear();
      return out;
    }
    out.inliers = inliers;
    out.correspondences = static_cast<int>(src.size());
    out.keypointMatches = static_cast<int>(good.size());
    out.inlierRatio = static_cast<float>(inliers) / std::max(1, out.correspondences);
    if (out.inlierRatio < 0.25f) {
      out.reject = "inlier ratio " + std::to_string(out.inlierRatio);
      out.corners.clear();
      return out;
    }
    out.ok = true;
    return out;
  } catch (const std::exception&) {
    out.reject = "exception";
    return out;
  }
}

Verification GeometricVerifier::measure(const MarkerRecord& marker, const cv::UMat& frame,
                                       const cv::Rect& roi, const PatchGrid& query,
                                       const cv::Size& frameSize) {
  if (frame.empty())
    return Verification{};

  Verification diag;
  Verification denseResult;
  if (!cfg_.forceKeypoints) {
    denseResult = dense(marker, query, roi, frameSize, diag);
    if (denseResult.ok)
      return denseResult;
  }

  // The dense path declined. Either the marker was registered without a patch grid, or
  // DINOv3 could not see enough of it unambiguously this frame. ORB is worth trying in
  // the second case, because a marker that is a few dozen pixels across has no room for
  // a 14x14 patch grid.
  Verification keyResult = keypoints(marker, frame, cv::Rect(0, 0, frame.cols, frame.rows),
                                     frameSize);
  if (keyResult.ok) {
    keyResult.which = Localiser::Keypoints;
    return keyResult;
  }
  // Only attribute the dense failure when there was one to blame. Repeating the same
  // sentence twice tells the reader nothing about which stage was reached.
  if (!cfg_.forceKeypoints && denseResult.reject != keyResult.reject)
    keyResult.reject += " (dense: " + denseResult.reject + ")";
  return keyResult;
}

bool GeometricVerifier::decompose(const std::vector<cv::Point2f>& q, Pose& pose) {
  if (q.size() != 4)
    return false;
  pose = Pose{};
  pose.centre = centroid(q);
  const double area = std::abs(signedArea(q));
  if (area < 1.0)
    return false;
  pose.scale = static_cast<float>(std::sqrt(area));
  for (size_t i = 0; i < 4; ++i)
    pose.shape[i] = (q[i] - pose.centre) / pose.scale;
  return true;
}

std::vector<cv::Point2f> GeometricVerifier::compose(const Pose& pose) {
  std::vector<cv::Point2f> q(4);
  for (size_t i = 0; i < 4; ++i)
    q[i] = pose.centre + pose.shape[i] * pose.scale;
  return q;
}

Verification GeometricVerifier::verify(const MarkerRecord& marker, const cv::UMat& frame,
                                      const cv::Rect& roi, const PatchGrid& query,
                                      const cv::Size& frameSize) {
  // A shape remembered for one marker says nothing about another, and a resized frame
  // invalidates the pixel positions. Either way, start over.
  if (marker.id != trackedId_) {
    trackedId_ = marker.id;
    seeded_ = false;
    held_ = 0;
  }
  if (!frameSize.empty() && frameSize != frame_) {
    frame_ = frameSize;
    seeded_ = false;
    held_ = 0;
  }

  const Verification measured = measure(marker, frame, roi, query, frameSize);

  Pose next;
  const bool usable = measured.ok && decompose(measured.corners, next);

  if (!usable) {
    // Nothing trustworthy this frame. Keep the previous pose alive for a few frames --
    // a marker that blinks out for two frames should not strobe -- but do not let it
    // live forever.
    if (seeded_) {
      ++held_;
      if (held_ > cfg_.holdFrames) {
        seeded_ = false;
        pose_ = Pose{};
      } else {
        Verification out = measured;
        out.ok = true;
        out.stale = true;
        out.corners = compose(pose_);
        // Report the quality of what is on screen. Nothing was measured this frame, so
        // that is zero, not the last good fit's numbers.
        out.inliers = 0;
        out.correspondences = 0;
        out.inlierRatio = 0.0f;
        return out;
      }
    }
    Verification out = measured;
    out.ok = false;
    out.corners.clear();
    return out;
  }

  if (!seeded_) {
    // First good frame for this marker: adopt the measurement outright. There is no
    // history to blend with, and a filter that eases in would be visible as a slow
    // drift on startup.
    pose_ = next;
    seeded_ = true;
    held_ = 0;
  } else {
    // How much to trust this measurement, from its own inlier ratio. A shaky fit moves
    // the filter less than a solid one, so a bad frame cannot yank the outline even if
    // it passes the structural checks.
    const float confidence =
        std::clamp(measured.inlierRatio / std::max(1e-3f, cfg_.minInlierRatio * 2.0f), 0.05f,
                   1.0f);

    // The shape moves at most maxShapeStep of the marker's own size in one frame. A
    // rigid marker cannot rotate fast enough to exceed that, so anything which does is a
    // bad estimate and is clamped rather than blended in -- this is the step that turns
    // a single-frame wild jump into a bounded one.
    for (size_t i = 0; i < 4; ++i) {
      const cv::Point2f delta = next.shape[i] - pose_.shape[i];
      const float step = cv::norm(delta);
      // Scaled by how far the corner already sits from the centre, so the limit is
      // "this fraction of the marker's own size" rather than an absolute number.
      const float reach = std::max(0.25f, static_cast<float>(cv::norm(pose_.shape[i])));
      const float limit = cfg_.maxShapeStep * reach;
      const float alpha = cfg_.shapeSmoothing * confidence;
      const float t = (step > limit && step > 0.0f) ? limit / step : alpha;
      pose_.shape[i] += delta * t;
    }
    // Position and size genuinely move every frame, so they are followed rather than
    // smoothed: lagging them would put the outline visibly behind the marker.
    pose_.centre += (next.centre - pose_.centre) * cfg_.centreSmoothing;
    if (pose_.scale > 0.0f && next.scale > 0.0f) {
      // Log-domain so that growing and shrinking are treated symmetrically.
      pose_.scale *= std::exp(std::log(next.scale / pose_.scale) * cfg_.scaleSmoothing);
    }
    held_ = 0;
  }

  Verification out = measured;
  out.corners = compose(pose_);
  out.ok = true;
  return out;
}