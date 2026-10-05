// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
//
// Dense localisation from DINOv3 patch tokens.
//
// The header explains what this is for and why the correspondence stage is built
// the way it is. This file is the implementation, in five steps:
//
//   cosineMatrix()   one gemm: cos(marker patch, frame patch) for all pairs
//   contextFilter()  a separable binomial window over both patch grids
//   similarity()     top-2 with non-maximum suppression, plus the reverse map
//   vote()           deterministic 4-point hypotheses, scored over every candidate
//   refine()         MAGSAC++ over the winner's support, then the RoI corners
//
// match() runs the first four in order and hands the surviving correspondences to
// refine(). similarity() and vote() are separate because the distributions and the
// vote margin are the measurement the thresholds rest on, and the self-test dumps
// both.

#include "dinov3-patch-matcher.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

// findHomography() lives in the geometry module in OpenCV 5; see the note in
// dinov3-geometric-verifier.cpp.
#include <opencv2/geometry/3d.hpp>

namespace cv {
namespace samples {
namespace dinov3 {

namespace {

// The marker's crop corners, clockwise from the top-left, in the marker's own
// crop pixels. This is the space the reference patch grid was measured in, so
// the homography takes them straight to the current frame's crop.
std::vector<Point2f> cropCorners(const Size2f &size) {
  return {Point2f(0.0f, 0.0f), Point2f(size.width, 0.0f),
          Point2f(size.width, size.height), Point2f(0.0f, size.height)};
}

// Geometry of a candidate outline, measured before any judgement is made on it.
//
// Every quantity is a fraction of the frame or a sign, never an absolute pixel count.
// A homography fitted to patch centres can satisfy an inlier test and still be
// nonsense -- collapsed to a line, folded into a bowtie, shrunk to a speck, or sent
// off to infinity -- and which of those happened is the whole diagnosis. So this
// reports the shape rather than a single accept/reject, and project() puts the
// numbers in the dump whether the answer was yes or no.
struct QuadShape {
  double area = 0.0;   // px^2, shoelace, unsigned
  double areaFraction = 0.0; // of the frame
  float minSpan = 0.0f;     // shortest corner-to-corner distance, px
  float minSide = 0.0f;     // shortest side, px
  bool finite = false;      // all four corners are real points
  bool convex = false;      // no fold: the four cross products agree in sign
  bool inBounds = false;    // inside the frame's bounds
};

QuadShape measureQuad(const std::vector<Point2f> &corners, Size frameSize) {
  QuadShape shape;
  if (corners.size() != 4)
    return shape;

  shape.finite = true;
  for (const Point2f &corner : corners)
    shape.finite &= std::isfinite(corner.x) && std::isfinite(corner.y);
  if (!shape.finite)
    return shape;

  double signedArea = 0.0;
  for (int i = 0; i < 4; ++i) {
    const Point2f &a = corners[i];
    const Point2f &b = corners[(i + 1) % 4];
    signedArea += (double)a.x * b.y - (double)b.x * a.y;
  }
  shape.area = std::abs(signedArea) * 0.5;
  const double frameArea = std::max(1.0, (double)frameSize.width * frameSize.height);
  shape.areaFraction = shape.area / frameArea;

  shape.minSpan = std::numeric_limits<float>::max();
  shape.minSide = std::numeric_limits<float>::max();
  for (int i = 0; i < 4; ++i) {
    shape.minSide = std::min(shape.minSide, (float)norm(corners[i] - corners[(i + 1) % 4]));
    for (int j = i + 1; j < 4; ++j)
      shape.minSpan = std::min(shape.minSpan, (float)norm(corners[i] - corners[j]));
  }

  // Convexity: for a simple quad in order, all four turns have the same sign. A
  // bowtie -- the outline folded over itself, which is what a projective fit does
  // when its denominator changes sign inside the region -- produces two of each.
  int turns = 0;
  for (int i = 0; i < 4; ++i) {
    const Point2f &a = corners[i];
    const Point2f &b = corners[(i + 1) % 4];
    const Point2f &c = corners[(i + 2) % 4];
    const double cross = (double)(b.x - a.x) * (c.y - b.y) -
                         (double)(b.y - a.y) * (c.x - b.x);
    if (std::abs(cross) < 1e-9)
      return shape; // a straight or doubled edge: not a quad
    if (cross > 0.0)
      ++turns;
  }
  shape.convex = (turns == 4 || turns == 0);

  shape.inBounds = true;
  for (const Point2f &corner : corners)
    shape.inBounds &= corner.x >= 0.0f && corner.y >= 0.0f &&
                      corner.x <= (float)frameSize.width &&
                      corner.y <= (float)frameSize.height;
  return shape;
}

// Percentile of a sample, by rank. Copies, so the caller's vector is untouched;
// only ever called on a few hundred floats and only when the caller asked for the
// distributions.
float percentile(std::vector<float> &values, float q) {
  if (values.empty())
    return 0.0f;
  std::sort(values.begin(), values.end());
  const size_t rank = (size_t)std::lround(q * (float)(values.size() - 1));
  return values[std::min(rank, values.size() - 1)];
}

// Binomial weights for a window of the given radius, normalised: [1, r, 1] gives
// the 3x3 kernel's centre twice the weight of its edges, [1, 4, 6, 4, 1] the
// binomial for radius 2. Binomial rather than a box so the centre of the window is
// the most trusted part of it -- a marker seen at an angle displaces its patches by
// up to a cell, and a window that leans on its centre is the one that survives that
// without smearing.
std::vector<double> binomialKernel(int radius) {
  const int n = 2 * radius + 1;
  std::vector<double> k((size_t)n, 1.0);
  for (int pass = 0; pass < radius; ++pass) {
    for (int i = 0; i < n - 1; ++i)
      k[(size_t)i] += k[(size_t)i + 1];
  }
  // The passes above produce an increasing run; the kernel is symmetric about its
  // centre, so mirror the first radius+1 entries.
  std::vector<double> out((size_t)n, 1.0);
  for (int i = 0; i <= radius; ++i) {
    out[(size_t)i] = out[(size_t)(n - 1 - i)] = k[(size_t)i];
  }
  double sum = 0.0;
  for (double v : out)
    sum += v;
  for (double &v : out)
    v /= sum;
  return out;
}

// Separable binomial filter over one axis of a grid-indexed matrix.
//
// `count` cells along that axis, `other` cells on the other axis, `stride` how far
// one cell along this axis moves in memory. Cells outside the grid are dropped and
// the window renormalised, so a patch on the marker's border is not penalised for
// it.
void filterAxis(const std::vector<float> &in, std::vector<float> &out, int count,
                int other, int stride, const std::vector<double> &kernel) {
  const int radius = (int)(kernel.size() - 1) / 2;
  out.resize(in.size());
  for (int a = 0; a < count; ++a) {
    const size_t base = (size_t)a * stride;
    for (int b = 0; b < other; ++b) {
      double acc = 0.0, weight = 0.0;
      for (int d = -radius; d <= radius; ++d) {
        const int cell = a + d;
        if (cell < 0 || cell >= count)
          continue;
        const double w = kernel[(size_t)(d + radius)];
        acc += w * in[(size_t)cell * stride + b];
        weight += w;
      }
      out[base + b] = weight > 0.0 ? (float)(acc / weight) : 0.0f;
    }
  }
}

// Least-squares affine fit: u = a.x + b.y + c, v = d.x + e.y + f.
//
// Two independent 3-unknown normal equations, but solved in centred coordinates. On
// raw pixel coordinates the constant term competes with two large linear terms in one
// 3x3 solve, and the condition number here is exactly the sort of thing that silently
// costs a digit of accuracy. Centring on the centroid makes the coordinates
// independent and the solve well posed; the fit it describes is the same fit.
//
// The centred design has one fewer unknown per channel -- there is no intercept left,
// because a centred coordinate has zero mean -- so each channel is a 2x2 solve against
// the Gram matrix [[sxx, sxy], [sxy, syy]], and the intercept is recovered afterwards as
// the mean of the targets.
bool fitAffineLeastSquares(const std::vector<Point2f> &src,
                           const std::vector<Point2f> &dst,
                           const std::vector<int> &use, Mat &out) {
  const size_t n = use.size();
  if (n < 3)
    return false;

  double mx = 0.0, my = 0.0;
  for (int i : use) {
    mx += src[(size_t)i].x;
    my += src[(size_t)i].y;
  }
  mx /= (double)n;
  my /= (double)n;

  double sxx = 0.0, sxy = 0.0, syy = 0.0, mu = 0.0, mv = 0.0;
  for (int i : use) {
    const double x = src[(size_t)i].x - mx;
    const double y = src[(size_t)i].y - my;
    sxx += x * x;
    sxy += x * y;
    syy += y * y;
    mu += dst[(size_t)i].x;
    mv += dst[(size_t)i].y;
  }
  mu /= (double)n;
  mv /= (double)n;

  const double det = sxx * syy - sxy * sxy;
  if (std::abs(det) < 1e-9)
    return false; // the points are collinear; an affine map is not determined

  // Per-channel right-hand sides: the cross terms of the centred targets.
  double rux = 0.0, ruy = 0.0, rvx = 0.0, rvy = 0.0;
  for (int i : use) {
    const double x = src[(size_t)i].x - mx;
    const double y = src[(size_t)i].y - my;
    const double du = dst[(size_t)i].x - mu;
    const double dv = dst[(size_t)i].y - mv;
    rux += x * du;
    ruy += y * du;
    rvx += x * dv;
    rvy += y * dv;
  }

  // Inverse of the 2x2 Gram matrix, applied to each channel.
  const double a = (syy * rux - sxy * ruy) / det;
  const double b = (sxx * ruy - sxy * rux) / det;
  const double d = (syy * rvx - sxy * rvy) / det;
  const double e = (sxx * rvy - sxy * rvx) / det;
  if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(d) ||
      !std::isfinite(e))
    return false;

  // Undo the centring: x = X + mx and y = Y + my, so c = mu - a*mx - b*my.
  out = Mat::zeros(3, 3, CV_64F);
  out.at<double>(0, 0) = a;
  out.at<double>(0, 1) = b;
  out.at<double>(0, 2) = mu - a * mx - b * my;
  out.at<double>(1, 0) = d;
  out.at<double>(1, 1) = e;
  out.at<double>(1, 2) = mv - d * mx - e * my;
  out.at<double>(2, 2) = 1.0;
  return true;
}

// Count how many of the correspondences a transform explains, at `threshold`, into
// `support`.
int countReprojectionInliers(const Mat &model, const std::vector<Point2f> &src,
                             const std::vector<Point2f> &dst, double threshold,
                             std::vector<int> *support) {
  if (support)
    support->clear();
  if (model.empty() || src.size() != dst.size())
    return 0;
  const double thresholdSq = threshold * threshold;
  std::vector<Point2f> projected;
  try {
    perspectiveTransform(src, projected, model);
  } catch (const std::exception &) {
    return 0;
  }
  int inliers = 0;
  for (size_t i = 0; i < src.size(); ++i) {
    const double dx = projected[i].x - dst[i].x;
    const double dy = projected[i].y - dst[i].y;
    if (std::isfinite(dx) && std::isfinite(dy) && dx * dx + dy * dy <= thresholdSq) {
      ++inliers;
      if (support)
        support->push_back((int)i);
    }
  }
  return inliers;
}

// Robust affine fit over the support set.
//
// Seeded from a homography's inliers, then re-fitted and re-scored twice. It is a
// deliberate stop rather than a full M-estimator because the seed is already good --
// the point of this stage is to drop the projective term, not to rediscover the
// inlier set -- and each round is a 3-unknown solve over a few dozen points.
bool fitAffineRobust(const std::vector<Point2f> &src, const std::vector<Point2f> &dst,
                     double threshold, const std::vector<int> *seed, Mat &out,
                     int &inliers) {
  std::vector<int> use;
  if (seed && (int)seed->size() >= 3)
    use = *seed;
  else
    for (size_t i = 0; i < src.size(); ++i)
      use.push_back((int)i);

  out.release();
  inliers = 0;
  for (int round = 0; round < 3; ++round) {
    // (the round cap is a stop, not a search: see the comment above)
    Mat candidate;
    if (!fitAffineLeastSquares(src, dst, use, candidate))
      return false;
    std::vector<int> support;
    const int count = countReprojectionInliers(candidate, src, dst, threshold, &support);
    if (count < 3)
      return false;
    // Stop when the inlier set stops moving; otherwise keep the larger of the two so
    // a bad round cannot shrink the support and lock in a worse fit.
    if (support.size() == use.size() && support == use) {
      out = candidate;
      inliers = count;
      return true;
    }
    if (count >= inliers) {
      out = candidate;
      inliers = count;
    }
    if (support.size() < use.size())
      use = support; // never grow on a round: only a tighter consensus is a better seed
  }
  return !out.empty();
}

// Fit one model and report how much of the support set it explains, at `threshold`.
//
// Returns false only if the estimator produced nothing at all. The inlier count is
// the whole point: comparing two models on the *same* points with the *same*
// tolerance is the only fair test between them, and inliers over a common set is a
// direct, cheap, unbiased comparison -- far better than comparing residual sums,
// which the model with more freedom can always shrink.
bool fitAndScore(const std::vector<Point2f> &src, const std::vector<Point2f> &dst,
                 int method, double threshold, Mat &out, int &inliers) {
  out.release();
  inliers = 0;
  if (src.size() < 4)
    return false;

  Mat mask;
  Mat model;
  try {
    model = findHomography(src, dst, method, threshold, mask, 2000, 0.995);
  } catch (const std::exception &) {
    try {
      model = findHomography(src, dst, RANSAC, threshold, mask, 2000, 0.995);
    } catch (const std::exception &) {
      return false;
    }
  }
  if (model.empty() || model.rows != 3 || model.cols != 3)
    return false;

  inliers = (!mask.empty() && mask.type() == CV_8U) ? countNonZero(mask)
                                                    : (int)src.size();
  out = model;
  return true;
}

// The affine part of a 3x3 homography: its first two rows over a normalised bottom
// row. This is the 6-DOF model, obtained from an 8-DOF fit by discarding exactly the
// term that runs away.
Mat affinePart(const Mat &h) {
  Mat affine = Mat::eye(3, 3, CV_64F);
  if (h.rows == 3 && h.cols == 3) {
    h.row(0).copyTo(affine.row(0));
    h.row(1).copyTo(affine.row(1));
  }
  return affine;
}

// Absolute area of a quadrilateral, for the degeneracy test on a seed. Kept local
// rather than using contourArea(): that one re-derives an orientation from the
// first point and is a heavier call than four cross products.
double polygonArea(const std::vector<Point2f> &points) {
  double area = 0.0;
  for (size_t i = 0; i < points.size(); ++i) {
    const Point2f &a = points[i];
    const Point2f &b = points[(i + 1) % points.size()];
    area += (double)a.x * b.y - (double)b.x * a.y;
  }
  return area * 0.5;
}

// cos(marker patch i, frame patch j) for every pair.
//
// Both operands are L2-normalised rows, so this is a cosine and nothing else.
// The operands are NxK and KxN, which is exactly the plain gemm contract, so no
// transposing flag belongs here: passing GEMM_2_T while also handing over an
// already-transposed B makes gemm reinterpret the KxN operand as NxK and demand
// A's width be N; OpenCV answers "Assertion failed (a_size.width == len)", the
// exception is swallowed by the caller, and the dense path silently declines every
// single frame.
bool cosineMatrix(const Mat &markerPatches, const Mat &framePatches, Mat &sim,
                  std::string &error) {
  const Mat frameT = framePatches.t();
  if (markerPatches.cols != frameT.rows) {
    error = "the marker and the frame disagree on descriptor size";
    return false;
  }
  try {
    gemm(markerPatches, frameT, 1.0, noArray(), 0.0, sim);
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
  if (sim.dims != 2 || sim.rows != markerPatches.rows ||
      sim.cols != framePatches.rows) {
    error = "the similarity matrix came out the wrong shape";
    return false;
  }
  if (sim.type() != CV_32F)
    sim.convertTo(sim, CV_32F);
  return true;
}

// The neighbourhood filter. See point 1 in the header: replaces each similarity by
// the binomial-weighted mean over a (2r+1)^2 window of the two patch grids, which
// sharpens the smooth ridge a real correspondence forms and leaves an isolated
// spurious peak as a spike.
//
// Separable, so it costs two passes over the matrix rather than one pass of
// (2r+1)^2 times it, and written over a flat buffer with the grid strides spelled
// out -- there is no 2D filtering call here because the two grids generally have
// different shapes and OpenCV's would need them equal.
bool contextFilter(const Mat &sim, Size markerGrid, Size frameGrid, int radius,
                   Mat &out, std::string &error) {
  if (radius <= 0) {
    out = sim;
    return true;
  }
  if (markerGrid.width <= 0 || markerGrid.height <= 0 || frameGrid.width <= 0 ||
      frameGrid.height <= 0) {
    error = "a patch grid has no shape";
    return false;
  }
  const int markerCells = markerGrid.width * markerGrid.height;
  const int frameCells = frameGrid.width * frameGrid.height;
  if (markerCells != sim.rows || frameCells != sim.cols) {
    error = "the patch grids do not account for every token";
    return false;
  }

  const std::vector<double> kernel = binomialKernel(radius);
  std::vector<float> a((size_t)sim.rows * sim.cols), b;
  std::memcpy(a.data(), sim.ptr<float>(), a.size() * sizeof(float));

  // Marker axis: for each (marker cell, frame patch), slide over the marker's rows.
  filterAxis(a, b, markerGrid.height, markerGrid.width * frameCells,
             markerGrid.width, kernel);
  // Frame axis: for each (marker patch, frame cell), slide over the frame's rows.
  filterAxis(b, a, frameGrid.height, markerCells, 1, kernel);

  out.create((int)markerCells, (int)frameCells, CV_32F);
  std::memcpy(out.ptr<float>(), a.data(), a.size() * sizeof(float));
  return true;
}

} // namespace

PatchMatcher::PatchMatcher(const PatchMatchOptions &options) {
  setOptions(options);
}

void PatchMatcher::setOptions(const PatchMatchOptions &options) {
  options_ = options;
  // A non-maximum radius below 1 would mean "the runner-up may be a neighbour of
  // the winner", which is the defect the disc exists to remove. Clamp both knobs so
  // no combination of them can quietly reintroduce it.
  options_.contextRadius = std::max(0, options_.contextRadius);
  options_.nmsRadius = std::max(1, options_.nmsRadius);
  options_.voteHypotheses = std::max(0, options_.voteHypotheses);
  options_.minMatches = std::max(4, options_.minMatches);
  options_.minInliers = std::max(4, options_.minInliers);
  options_.voteThresholdCells = std::max(0.25f, options_.voteThresholdCells);
  // No floor beyond a positive value: this one is a statement about how precisely
  // the fit must place a patch centre, and a user asking for 0.1 cells means it.
  options_.refineThresholdCells = std::max(0.05f, options_.refineThresholdCells);
}

const PatchMatchOptions &PatchMatcher::options() const { return options_; }

double PatchMatcher::lastMs() const { return timer_.getLastTimeMilli(); }

bool PatchMatcher::similarity(const DenseReference &reference,
                              const PatchGrid &framePatches, Similarity &out) {
  out = Similarity();
  out.patches = reference.patches.rows;
  out.framePatches = framePatches.descriptors.rows;

  if (out.patches <= 0 || out.framePatches <= 0) {
    out.error = "there are no patch descriptors to match";
    return false;
  }

  Mat sim;
  if (!cosineMatrix(reference.patches, framePatches.descriptors, sim, out.error))
    return false;

  if (!contextFilter(sim, reference.grid, framePatches.grid,
                     options_.contextRadius, out.context, out.error))
    return false;

  const int frameCols = framePatches.grid.width;
  const int nms = options_.nmsRadius;

  // Best, and the best outside a disc of `nms` cells around it, plus the reverse
  // lookup for the mutual check. Keeping the reverse map is what stops a patch that
  // is the runner-up everywhere from being matched repeatedly.
  //
  // Both inner loops are over a contiguous row and run once per marker patch; that
  // is the whole of the matcher's cost now that the embedding is free.
  out.bestIndex.resize(out.patches);
  out.secondIndex.resize(out.patches);
  out.bestCos.resize(out.patches);
  out.secondCos.resize(out.patches);
  out.bestCtx.resize(out.patches);
  out.secondCtx.resize(out.patches);
  out.bestOfFrame.assign(out.framePatches, -1);
  std::vector<float> bestOfFrameCtx((size_t)out.framePatches, -2.0f);

  const float *raw = sim.ptr<float>();
  for (int i = 0; i < out.patches; ++i) {
    const float *row = out.context.ptr<float>(i);
    const float *rawRow = raw + (size_t)i * out.framePatches;

    int best = -1, second = -1;
    float bestCtx = -2.0f, secondCtx = -2.0f;
    for (int j = 0; j < out.framePatches; ++j) {
      const float s = row[j];
      if (s > bestCtx) {
        bestCtx = s;
        best = j;
      }
    }
    if (best >= 0) {
      const int br = best / frameCols, bc = best % frameCols;
      for (int j = 0; j < out.framePatches; ++j) {
        const int r = j / frameCols, c = j % frameCols;
        if (std::abs(r - br) <= nms && std::abs(c - bc) <= nms)
          continue; // inside the disc: not a competing hypothesis
        const float s = row[j];
        if (s > secondCtx) {
          secondCtx = s;
          second = j;
        }
      }
    }

    out.bestIndex[i] = best;
    out.secondIndex[i] = second;
    out.bestCtx[i] = best >= 0 ? bestCtx : -1.0f;
    out.secondCtx[i] = second >= 0 ? secondCtx : -1.0f;
    out.bestCos[i] = best >= 0 ? rawRow[best] : -1.0f;
    out.secondCos[i] = second >= 0 ? rawRow[second] : -1.0f;

    if (best >= 0 && bestCtx > bestOfFrameCtx[(size_t)best]) {
      bestOfFrameCtx[(size_t)best] = bestCtx;
      out.bestOfFrame[(size_t)best] = i;
    }
  }
  return true;
}

bool PatchMatcher::buildCandidates(const DenseReference &reference,
                                   const PatchGrid &framePatches, Rect roi,
                                   const Similarity &sim, Candidates &out,
                                   PatchMatchDiagnostics &diag) const {
  const Size frameGrid = framePatches.grid;
  const Size2f frameCrop((float)roi.width, (float)roi.height);

  diag.patches = sim.patches;
  diag.aboveFloor = 0;
  diag.afterRatio = 0;
  diag.mutual = 0;

  for (int i = 0; i < sim.patches; ++i) {
    const int j = sim.bestIndex[i];
    if (j < 0 || sim.bestCtx[i] < options_.minSimilarity)
      continue;
    ++diag.aboveFloor;

    if (options_.useRatioTest) {
      const int k = sim.secondIndex[i];
      if (k < 0)
        continue;
      const float bestDistance = 1.0f - sim.bestCtx[i];
      const float secondDistance = 1.0f - sim.secondCtx[i];
      if (secondDistance <= 0.0f)
        continue;
      if (bestDistance >= options_.ratioThreshold * secondDistance)
        continue;
      ++diag.afterRatio;
    }

    if (options_.useMutualCheck) {
      if (sim.bestOfFrame[(size_t)j] != i)
        continue;
      ++diag.mutual;
    }

    out.markerPoints.push_back(patchCentre(i, reference.grid, reference.roiSize));
    out.framePoints.push_back(patchCentre(j, frameGrid, frameCrop));
    out.candidates.push_back(i);
  }
  return !out.candidates.empty();
}

bool PatchMatcher::vote(const Candidates &candidates, const GridSpace &space,
                        Mat &h, std::vector<unsigned char> &support,
                        PatchMatchDiagnostics &diag) const {
  const int n = (int)candidates.candidates.size();
  if (n < options_.minMatches)
    return false;

  // Everything here is measured in the *marker's* crop pixels, because that is the
  // one space the hypotheses and the candidates already share -- a marker cell and
  // a frame cell are different sizes whenever the marker covers a different
  // fraction of the two crops, and converting between them per hypothesis is both
  // needless and a chance to get it wrong.
  //
  // The tolerance is one patch cell, scaled by voteThresholdCells: a patch is worth
  // one cell, so half a cell of disagreement is already a different patch.
  const double tolerance =
      std::max(1e-6, space.cell) * options_.voteThresholdCells;
  const double toleranceSq = tolerance * tolerance;

  std::vector<Point2f> projected;
  std::vector<unsigned char> mask((size_t)n, 0);

  auto score = [&](const Mat &candidate) -> int {
    if (candidate.empty() || candidate.rows != 3 || candidate.cols != 3)
      return -1;
    // A vanishing third row maps everything onto a line: a degenerate answer rather
    // than a wrong one, so it is skipped rather than scored.
    if (std::abs(candidate.at<double>(2, 2)) < 1e-12)
      return -1;
    try {
      perspectiveTransform(candidates.markerPoints, projected, candidate);
    } catch (const std::exception &) {
      return -1;
    }
    int count = 0;
    for (int i = 0; i < n; ++i) {
      const double dx = projected[i].x - candidates.framePoints[i].x;
      const double dy = projected[i].y - candidates.framePoints[i].y;
      if (!std::isfinite(dx) || !std::isfinite(dy))
        return -1;
      if (dx * dx + dy * dy <= toleranceSq) {
        mask[(size_t)i] = 1;
        ++count;
      } else {
        mask[(size_t)i] = 0;
      }
    }
    return count;
  };

  // Deterministic hypothesis generation, and the seed geometry is the whole thing.
  //
  // Four seed points have to span the marker. Four *adjacent* ones are nearly
  // collinear, which gives a transform with an almost unconstrained denominator --
  // one that maps most of the grid into tolerance while being nonsense, so it wins
  // the vote on support count and produces a wild outline. That is not hypothetical:
  // the seeds used to be four consecutive slices of the candidate list at a stride
  // of `n / voteHypotheses` = 6, which on a 14-wide grid advances 0.43 of a row per
  // step, so they spanned 1.3 rows. The area test threw out 21 of the 30, and the 9
  // survivors were the seeds that happened to straddle a row boundary: 2 rows by 5
  // columns of a 14x14 grid. `votes_tried` is 9 on every frame of the shipped clip,
  // which is that arithmetic made visible, and the winner supported 160 of 196
  // correspondences with the runner-up on 160.
  //
  // Striding the list does not work at all, for a second reason: the list is
  // row-major, so a stride that is not a multiple of the grid width walks the seeds
  // across the rows in a zig-zag. Four points picked that way in index order --
  // (0,0), (3,7), (7,0), (10,7) at a stride of 49 -- form a self-intersecting
  // quadrilateral whose shoelace area is identically zero, and the area test then
  // rejects all of them.
  //
  // So the seeds are chosen in grid coordinates instead: the candidates extremal in a
  // rotated frame, `u = r*cos(t) + c*sin(t)` and `v = -r*sin(t) + c*cos(t)`. At t = 0
  // that is exactly the marker's four corners; sweeping t over a quarter turn gives
  // `voteHypotheses` genuinely different quads, all of them spanning the marker's
  // extent, all of them simple. Choosing extremes rather than "every k-th point"
  // also means the seeds stay spread when the candidate set is a sparse subset of
  // the grid, which is the case that matters -- the floor does not pass all 196
  // patches on a hard frame.
  //
  // No randomness anywhere: the same frame always produces the same answer, so a
  // regression in the dump is a regression in the code and not in the seed.
  int bestCount = -1, runnerUpCount = -1, supportedCount = 0;
  std::vector<unsigned char> bestMask;
  Mat best;

  const int gridWidth = std::max(1, space.grid.width);
  // Row and column of every candidate on the marker's grid.
  std::vector<Point2f> gridPos((size_t)n);
  for (int i = 0; i < n; ++i) {
    const int patch = candidates.candidates[(size_t)i];
    gridPos[(size_t)i] =
        Point2f((float)(patch % gridWidth), (float)(patch / gridWidth));
  }

  // A quarter turn is the period of the rotation -- beyond it the four corners
  // repeat -- so the family is (rotations x insets), and insets matter as much as
  // rotations. Sweeping rotation alone was tried and is not enough: on a 14x14 grid
  // the extremal four points only change when the rotation passes a tie, so most of
  // the sweep selects the *same* quad. That does not merely waste the hypotheses,
  // it corrupts the margin, because the runner-up then has the winner's score by
  // construction -- `vote_runner_up_inliers` came out exactly equal to
  // `vote_best_inliers` on all 14 sampled frames, which is the signature of a
  // duplicated hypothesis rather than of a genuine second opinion.
  //
  // Insets give the second axis of variety: the same rectangle at 0, a third, two
  // thirds and all the way in gives four genuinely different quads per rotation,
  // from the marker's full extent down to its middle third. The inner ones are what
  // a marker that fills only part of the crop needs, and they are well conditioned
  // too, being further from collinear than any stride of the candidate list.
  const int rotations = std::max(1, options_.voteHypotheses / 4);
  static const double kInsets[4] = {0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0};
  int tried = 0;

  for (int ri = 0; ri < rotations; ++ri) {
    const double theta = (M_PI / 2.0) * (double)ri / (double)rotations;
    const double ct = std::cos(theta), st = std::sin(theta);

    // The rotated frame's extent over all candidates.
    double uLo = 0, uHi = 0, vLo = 0, vHi = 0;
    for (int i = 0; i < n; ++i) {
      const double u = gridPos[(size_t)i].x * ct + gridPos[(size_t)i].y * st;
      const double v = -gridPos[(size_t)i].x * st + gridPos[(size_t)i].y * ct;
      if (i == 0 || u < uLo) uLo = u;
      if (i == 0 || u > uHi) uHi = u;
      if (i == 0 || v < vLo) vLo = v;
      if (i == 0 || v > vHi) vHi = v;
    }

    for (double inset : kInsets) {
      // The four corners of an axis-aligned rectangle of the rotated grid, shrunk
      // towards its middle by `inset`, walked in cyclic order.
      const double u0 = uLo + inset * (uHi - uLo), u1 = uHi - inset * (uHi - uLo);
      const double v0 = vLo + inset * (vHi - vLo), v1 = vHi - inset * (vHi - vLo);

      int seed[4];
      bool degenerate = false;
      for (int corner = 0; corner < 4; ++corner) {
        const double tu = (corner == 0 || corner == 3) ? u0 : u1;
        const double tv = (corner < 2) ? v0 : v1;
        int best = -1;
        double bestD = 0;
        for (int i = 0; i < n; ++i) {
          const double u = gridPos[(size_t)i].x * ct + gridPos[(size_t)i].y * st;
          const double v = -gridPos[(size_t)i].x * st + gridPos[(size_t)i].y * ct;
          const double d = (u - tu) * (u - tu) + (v - tv) * (v - tv);
          if (best < 0 || d < bestD) { bestD = d; best = i; }
        }
        // Two corners of a thin candidate set can land on the same patch, and a
        // repeated point makes the fit meaningless rather than merely bad. There is
        // no deduplication sort here on purpose: the four indices are already in
        // cyclic order, and sorting them by marker-patch index -- row-major over the
        // grid -- reorders them into (r0c0, r0c1, r1c0, r1c1), which is a bowtie. Its
        // shoelace area is identically zero, so the area test below would reject
        // every hypothesis that went through one. It silently rejected 28 of the 32
        // before this was noticed, leaving 4 arbitrary survivors.
        for (int other = 0; other < corner; ++other) {
          if (seed[other] == best)
            degenerate = true;
        }
        seed[corner] = best;
      }
      if (degenerate)
        continue;

      std::vector<Point2f> src, dst;
      src.reserve(4);
      dst.reserve(4);
      for (int k = 0; k < 4; ++k) {
        src.push_back(candidates.markerPoints[(size_t)seed[k]]);
        dst.push_back(candidates.framePoints[(size_t)seed[k]]);
      }
      // The area test, not a try/catch: four points from a 14x14 grid are almost
      // never degenerate, and findHomography throws rather than returning an empty
      // matrix for the ones that are.
      if (std::abs(polygonArea(src)) < 1.0)
        continue;
      ++tried;

      Mat candidate;
      try {
        candidate = findHomography(src, dst, 0);
      } catch (const std::exception &) {
        continue;
      }
      const int count = score(candidate);
      if (count < 0)
        continue;
      if (count >= options_.minInliers)
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
  diag.voteBestInliers = std::max(0, bestCount);
  diag.voteRunnerUpInliers = std::max(0, runnerUpCount);

  if (bestCount < options_.minInliers || best.empty())
    return false;

  h = best;
  support = std::move(bestMask);
  return true;
}

bool PatchMatcher::project(const DenseReference &reference, const Mat &h, Rect roi,
                           Size frameSize, VerifyResult &result,
                           PatchMatchDiagnostics &diag) const {
  // The crop corners, in the marker's own crop pixels, are the only points outside
  // the patch grid: the grid measures patch *centres*, so its outermost centres sit
  // half a cell inside the crop. Sending the four corners through the fitted
  // transform is what turns a set of correspondences into an outline.
  const std::vector<Point2f> crop = cropCorners(reference.roiSize);

  std::vector<Point2f> corners;
  QuadShape shape;
  try {
    perspectiveTransform(crop, corners, h);
  } catch (const std::exception &e) {
    result.error = e.what();
    return false;
  }
  for (Point2f &corner : corners)
    corner += Point2f((float)roi.x, (float)roi.y);
  shape = measureQuad(corners, frameSize);

  // The projective part is the least constrained thing about the fit: it is
  // determined by how the patch grid recedes, and a grid whose patches are 50px apart
  // barely constrains it at all. When the denominator of the transform changes sign
  // inside the marker's own extent, the corners leave the frame or go non-finite
  // while the patch centres it was fitted to still agree -- an outline that is a
  // consequence of a well-conditioned fit being extrapolated past where it is known.
  //
  // The affine part of the same transform, its first two rows, cannot do that: it has
  // no denominator to change sign. It is the correct answer whenever the true view is
  // only weakly projective, which is most views of a marker this close to the camera,
  // and it is strictly better than declaring failure when the corners are unusable.
  if ((!shape.finite || !shape.convex) && h.rows == 3 && h.cols == 3) {
    const Mat affine = affinePart(h);
    std::vector<Point2f> affineCorners;
    try {
      perspectiveTransform(crop, affineCorners, affine);
    } catch (const std::exception &) {
      result.error = "the fitted transform cannot be projected";
      return false;
    }
    for (Point2f &corner : affineCorners)
      corner += Point2f((float)roi.x, (float)roi.y);
    const QuadShape affineShape = measureQuad(affineCorners, frameSize);
    if (affineShape.finite) {
      corners = std::move(affineCorners);
      shape = affineShape;
      diag.affineFallback = true;
    }
  }

  diag.projectedArea = shape.area;
  diag.areaFraction = shape.areaFraction;
  diag.minSpan = shape.minSpan;
  diag.finite = shape.finite;
  diag.convex = shape.convex;

  if (!shape.finite) {
    result.corners.clear();
    result.error = "the fitted transform sends the outline off to infinity";
    return false;
  }

  if (options_.requirePlausibleQuad) {
    const double frameWidth = std::max(1, frameSize.width);
    const bool bigEnough = shape.areaFraction >= options_.minQuadAreaFraction &&
                           shape.areaFraction <= options_.maxQuadAreaFraction &&
                           shape.minSpan >= options_.minQuadSpanFraction * frameWidth;
    const bool simple = !options_.requireConvexQuad || shape.convex;
    if (!bigEnough || !simple) {
      result.corners.clear();
      result.error = "the fitted marker is degenerate";
      return false;
    }
  }
  diag.plausible = true;

  result.corners = std::move(corners);
  result.verified = true;
  return true;
}

bool PatchMatcher::refine(const DenseReference &reference,
                          const Candidates &candidates,
                          const std::vector<unsigned char> &support,
                          const GridSpace &space, Rect roi, Size frameSize,
                          VerifyResult &result, PatchMatchDiagnostics &diag) const {
  // Only the correspondences the winner already supports are handed to the estimator.
  // That is the point of voting: the estimator gets a set it does not have to
  // re-decide, so its budget goes into refining the transform instead of into
  // rediscovering which correspondences are real. It still runs rather than a
  // least-squares fit, because four exact points fit exactly and noisily while the
  // median residual over the whole support set does not.
  std::vector<Point2f> src, dst;
  src.reserve(support.size());
  dst.reserve(support.size());
  for (size_t i = 0; i < support.size(); ++i) {
    if (!support[i])
      continue;
    src.push_back(candidates.markerPoints[i]);
    dst.push_back(candidates.framePoints[i]);
  }
  const int supported = (int)src.size();
  if (supported < 4) {
    result.error.clear();
    diag.decline = PatchMatchDiagnostics::Decline::TooFewSupported;
    return false;
  }

  // A fraction of one patch cell of the marker's crop -- refineThresholdCells --
  // floored at two pixels so that a very small RoI cannot turn the tolerance into a
  // sub-pixel test that nothing survives.
  const double threshold =
      std::max(2.0, std::max(1e-6, space.cell) * options_.refineThresholdCells);

  Mat homography, affine;
  int homographyInliers = 0, affineInliers = 0;
  const bool haveHomography =
      options_.geometry != PatchMatchOptions::GeometryModel::Affine &&
      fitAndScore(src, dst, USAC_MAGSAC, threshold, homography, homographyInliers);

  // The affine fit is seeded with the homography's inliers when there is one. That
  // seed is what makes it robust: it inherits an already-consensus set instead of
  // running its own search over a field that still contains every spurious
  // correspondence the vote let through.
  std::vector<int> seed;
  if (haveHomography) {
    Mat mask;
    try {
      findHomography(src, dst, USAC_MAGSAC, threshold, mask, 2000, 0.995);
    } catch (const std::exception &) {
    }
    if (!mask.empty() && mask.type() == CV_8U) {
      for (size_t i = 0; i < mask.total(); ++i)
        if (mask.at<unsigned char>((int)i) != 0)
          seed.push_back((int)i);
    }
  }
  const bool haveAffine =
      options_.geometry != PatchMatchOptions::GeometryModel::Homography &&
      fitAffineRobust(src, dst, threshold, haveHomography ? &seed : nullptr, affine,
                      affineInliers);

  // Model selection. The 8-DOF fit is only preferred when it explains materially more
  // of the support set than the 6-DOF one; otherwise the affine model wins, because
  // the two agree about where the marker is and only one of them is capable of
  // producing an outline no camera could have seen. Preferring the model with more
  // freedom is the wrong default when the extra freedom is fitted to noise.
  Mat chosen;
  int chosenInliers = 0;
  if (haveHomography && haveAffine) {
    const int margin = (int)std::ceil(options_.homographyMargin * supported);
    if (homographyInliers >= affineInliers + margin) {
      chosen = homography;
      chosenInliers = homographyInliers;
      diag.modelHomography = true;
    } else {
      chosen = affine;
      chosenInliers = affineInliers;
    }
  } else if (haveHomography) {
    chosen = homography;
    chosenInliers = homographyInliers;
    diag.modelHomography = true;
  } else if (haveAffine) {
    chosen = affine;
    chosenInliers = affineInliers;
  } else {
    result.error = "no transform could be fitted";
    diag.decline = PatchMatchDiagnostics::Decline::FitFailed;
    return false;
  }

  diag.homographyInliers = homographyInliers;
  diag.affineInliers = affineInliers;

  result.inliers = chosenInliers;
  result.matches = supported;
  result.inlierRatio = (float)chosenInliers / (float)std::max(1, supported);

  if (result.inliers < options_.minInliers ||
      result.inlierRatio < options_.minInlierRatio) {
    // Correspondences existed but they did not agree on one marker.
    result.error.clear();
    diag.decline = PatchMatchDiagnostics::Decline::InlierGate;
    return false;
  }

  const bool projected =
      project(reference, chosen, roi, frameSize, result, diag);
  if (!projected)
    diag.decline = PatchMatchDiagnostics::Decline::DegenerateQuad;
  return projected;
}

bool PatchMatcher::match(const DenseReference &reference,
                         const PatchGrid &framePatches, Rect roi,
                         Size frameSize, VerifyResult &result,
                         PatchMatchDiagnostics *diagnostics) {
  timer_.start();
  result = VerifyResult();
  PatchMatchDiagnostics local;
  PatchMatchDiagnostics &diag = diagnostics ? *diagnostics : local;
  diag = PatchMatchDiagnostics();

  if (!options_.enabled) {
    diag.decline = PatchMatchDiagnostics::Decline::Disabled;
    result.error = "dense matching is off";
    timer_.stop();
    result.lastMs = diag.ms = timer_.getLastTimeMilli();
    return false;
  }
  if (reference.empty() || framePatches.empty()) {
    diag.decline = PatchMatchDiagnostics::Decline::NoReference;
    result.error = "there are no patch descriptors to match";
    timer_.stop();
    result.lastMs = diag.ms = timer_.getLastTimeMilli();
    return false;
  }
  if (roi.width <= 0 || roi.height <= 0) {
    diag.decline = PatchMatchDiagnostics::Decline::BadGeometry;
    result.error = "the crop geometry is empty";
    timer_.stop();
    result.lastMs = diag.ms = timer_.getLastTimeMilli();
    return false;
  }

  Similarity sim;
  if (!similarity(reference, framePatches, sim)) {
    diag.decline = PatchMatchDiagnostics::Decline::NoSimilarity;
    result.error = sim.error;
    timer_.stop();
    result.lastMs = diag.ms = timer_.getLastTimeMilli();
    return false;
  }

  // The correspondence problem, threshold-free. Collected here because the
  // caller's thresholds only mean something next to the distribution they cut into,
  // and that is what the self-test prints.
  {
    std::vector<float> best = sim.bestCos;
    std::vector<float> second = sim.secondCos;
    diag.bestCosMin = percentile(best, 0.0f);
    diag.bestCosP50 = percentile(best, 0.5f);
    diag.bestCosMax = percentile(best, 1.0f);
    diag.secondCosP50 = percentile(second, 0.5f);
    diag.secondCosP90 = percentile(second, 0.9f);

    std::vector<float> bestCtx = sim.bestCtx;
    std::vector<float> secondCtx = sim.secondCtx;
    std::vector<float> gaps;
    gaps.reserve((size_t)sim.patches);
    for (int i = 0; i < sim.patches; ++i)
      gaps.push_back(sim.secondIndex[i] >= 0
                         ? sim.bestCtx[i] - sim.secondCtx[i]
                         : 0.0f);
    diag.bestCtxP50 = percentile(bestCtx, 0.5f);
    diag.secondCtxP50 = percentile(secondCtx, 0.5f);
    diag.ctxGapMin = percentile(gaps, 0.0f);
    diag.ctxGapP50 = percentile(gaps, 0.5f);
    diag.haveDistributions = true;
  }

  // The correspondence field itself, once, if asked. See the option's comment: the
  // funnel counters above cannot tell a real map of the marker from a smooth drift,
  // and that distinction decides whether anything downstream is worth tuning.
  if (!options_.similarityDumpPath.empty()) {
    const std::string path = options_.similarityDumpPath;
    options_.similarityDumpPath.clear(); // one frame, not every frame
    if (FILE *out = std::fopen(path.c_str(), "w")) {
      const int fw = std::max(1, framePatches.grid.width);
      const int fh = std::max(1, framePatches.grid.height);
      const int mw = std::max(1, reference.grid.width);
      std::fprintf(out, "# marker_grid %dx%d  frame_grid %dx%d\n",
                   reference.grid.width, reference.grid.height, fw, fh);
      std::fprintf(out, "# contextRadius %d  nmsRadius %d  floor %.4f\n",
                   options_.contextRadius, options_.nmsRadius,
                   (double)options_.minSimilarity);
      std::fprintf(out,
                   "marker_patch,marker_row,marker_col,frame_patch,frame_row,"
                   "frame_col,best_ctx,second_ctx,gap,best_cos,second_cos,"
                   "above_floor\n");
      for (int i = 0; i < sim.patches; ++i) {
        const int j = sim.bestIndex[i];
        const int k = sim.secondIndex[i];
        std::fprintf(out, "%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%d\n", i,
                     i / mw, i % mw, j, j >= 0 ? j / fw : -1, j >= 0 ? j % fw : -1,
                     (double)sim.bestCtx[i], (double)sim.secondCtx[i],
                     (double)(sim.bestCtx[i] - sim.secondCtx[i]),
                     (double)sim.bestCos[i], (double)sim.secondCos[i],
                     j >= 0 && sim.bestCtx[i] >= options_.minSimilarity ? 1 : 0);
        (void)k;
      }
      std::fclose(out);
    }
  }

  Candidates candidates;
  if (!buildCandidates(reference, framePatches, roi, sim, candidates, diag)) {
    diag.decline = PatchMatchDiagnostics::Decline::NoCandidates;
    timer_.stop();
    result.lastMs = diag.ms = timer_.getLastTimeMilli();
    result.error.clear(); // not an error: this simply is not the marker
    return false;
  }

  result.matches = diag.matches = (int)candidates.candidates.size();
  if (result.matches < options_.minMatches) {
    // Correspondences existed but not enough of them. Worth saying plainly, because
    // "0 of 196 cleared the floor" and "190 of 196 cleared it and the homography
    // rejected them" need very different fixes.
    diag.decline = PatchMatchDiagnostics::Decline::TooFewMatches;
    timer_.stop();
    result.lastMs = diag.ms = timer_.getLastTimeMilli();
    result.error.clear();
    return false;
  }

  // The marker's grid, and the size of the crop it was stretched over. Everything
  // from here on is measured in that crop's pixels, one patch cell at a time.
  GridSpace space;
  space.grid = reference.grid;
  space.size = reference.roiSize;
  space.cell = std::min(space.grid.width > 0 ? space.size.width / space.grid.width
                                             : 1.0f,
                        space.grid.height > 0 ? space.size.height / space.grid.height
                                              : 1.0f);

  std::vector<unsigned char> support((size_t)result.matches, 1);
  Mat h;
  if (options_.voteHypotheses > 0) {
    if (!vote(candidates, space, h, support, diag)) {
      diag.decline = PatchMatchDiagnostics::Decline::NoVote;
      timer_.stop();
      result.lastMs = diag.ms = timer_.getLastTimeMilli();
      result.error.clear();
      return false;
    }
  } else {
    // No voting: the filtered set is the support set and MAGSAC++ finds the
    // transform directly. This is the pre-voting behaviour, kept so the sweep can
    // measure what the vote buys.
    try {
      h = findHomography(candidates.markerPoints, candidates.framePoints, 0);
    } catch (const std::exception &) {
      diag.decline = PatchMatchDiagnostics::Decline::FitFailed;
      timer_.stop();
      result.lastMs = diag.ms = timer_.getLastTimeMilli();
      result.error = "no homography could be fitted";
      return false;
    }
  }

  const bool projected =
      refine(reference, candidates, support, space, roi, frameSize, result, diag);
  diag.inliers = result.inliers;
  diag.inlierRatio = result.inlierRatio;
  diag.accepted = result.verified;
  if (result.verified)
    diag.decline = PatchMatchDiagnostics::Decline::None;

  timer_.stop();
  result.lastMs = diag.ms = timer_.getLastTimeMilli();
  return projected;
}

} // namespace dinov3
} // namespace samples
} // namespace cv
