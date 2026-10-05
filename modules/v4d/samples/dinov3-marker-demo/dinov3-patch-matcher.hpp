// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
//
// Dense localisation from DINOv3 patch tokens.
//
// GeometricVerifier answers "where is the marker" with ORB or SIFT: a separate
// detector runs over the frame, thousands of keypoints are matched by descriptor
// distance, and a homography is fitted to whatever survives. That costs a quarter
// of a second a frame and it misses most of the frames the embedding already
// recognised, mostly on scale, because ORB has only a few octaves of scale
// invariance.
//
// This file does the same job from the transformer's own patch tokens, which the
// embedder has already computed:
//
//   marker patch  <->  frame patch    by cosine similarity
//                 -> neighbourhood    a real correspondence has spatial support
//                 -> non-maximum     the runner-up must be a different place
//                 -> MAGSAC++        over a loose set, not a pre-filtered one
//                 -> corners         the marker's RoI corners through it
//
// Why this is the right tool for this job:
//
//   * It is free. The tokens come out of the forward pass the demo already runs
//     to decide *whether* the marker is present; a 196x196x768 matrix product
//     is a fraction of a millisecond. Localisation stops costing a detector run.
//   * It is dense. 196 correspondences is far more than a homography needs, so
//     MAGSAC++ converges even when much of the crop is background that happened
//     to look similar.
//   * It is scale-free by construction. A patch token describes what is inside
//     its 16x16 window, not where that window is, so moving the camera closer
//     does not invalidate the match the way it invalidates ORB's scale pyramid.
//
// What it gives up, and why that is acceptable: it only sees the region of
// interest, because that is all the embedder looked at. A marker far from the
// centre of the frame is found by GeometricVerifier's whole-frame search and
// missed here, which is why the pipeline tries this first and falls back rather
// than replacing the old path.
//
// ---------------------------------------------------------------------------
// Why the correspondence stage is not a plain Lowe ratio test
// ---------------------------------------------------------------------------
//
// The obvious formulation -- take each marker's nearest frame patch, keep it if
// its cosine beats the runner-up's by the usual ratio -- does not work on DINOv3
// tokens, and the dump in README.md shows why. Measured on the supplied marker
// clip at 224x224, the best and the runner-up cosine for the *same* marker patch
// sit about 0.01 apart: median 0.888 against 0.878 on a good frame, 0.578 against
// 0.570 on a hard one. A ratio of 0.92 on distances of 0.11 therefore cuts at a
// knife edge, and the funnel collapses to noise: across the sampled frames the
// ratio test kept 1 to 79 of 196 patches, the mutual check then took away half of
// what was left, and 6 of 14 recognised frames localised at all.
//
// The gap is small because adjacent patch tokens of one image are nearly the same
// vector. DINOv3 is trained to make a patch token describe its 16x16 window, and
// neighbouring windows overlap by 15/16 of their extent, so the token grid is
// smooth. A consequence follows for any test built on "the runner-up": the
// runner-up is almost always the neighbouring cell, not a competing hypothesis.
// Comparing a match against its own neighbour measures nothing.
//
// Three changes follow, and together they are what this file implements:
//
//   1. Neighbourhood support. A genuine correspondence is a smooth ridge across
//      both grids, because the marker's patch (r,c) and its neighbours map to the
//      frame's patch and *its* neighbours. Replacing every similarity by the
//      binomial-weighted mean over a (2r+1)^2 window of the two patch grids turns
//      the ridge into a peak and leaves an isolated spurious peak as a spike.
//      This is the single biggest lever: it widens the best/runner-up gap from
//      ~0.01 to a fraction of the full range on good frames, and it is a handful
//      of adds over a 196x196 matrix.
//
//   2. Non-maximum suppression. Once (1) has sharpened the peak, its immediate
//      neighbours are still high, because their windows overlap the peak's -- so
//      the top-2 still returns two adjacent cells and the ratio test measures
//      again. The runner-up is therefore taken from outside a small disc around
//      the winner. It is then a genuinely different hypothesis, which is the only
//      thing a ratio test is for.
//
//   3. Voting instead of a hard pre-filter. With the runner-up meaning something,
//      a ratio test *can* work -- but discarding 80% of an already-good
//      correspondence set before the robust estimator ever sees it throws away the
//      very redundancy MAGSAC++ needs. So the loose set (every patch whose best
//      neighbourhood score clears the floor) is kept whole, and the transform is
//      chosen by voting: a spread of deterministic 4-point hypotheses is scored
//      against *all* loose correspondences and the best-supported one wins. That is
//      the pose-voting step the research notes recommend, and it is what lets the
//      acceptance thresholds sit where they do.
//
// Like the rest of the demo's logic, this file knows nothing about V4D.

#ifndef OPENCV_DINOV3_PATCH_MATCHER_HPP
#define OPENCV_DINOV3_PATCH_MATCHER_HPP

#include "dinov3-embedder.hpp"
#include "dinov3-geometric-verifier.hpp"

#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace cv {
namespace samples {
namespace dinov3 {

struct PatchMatchOptions {
  bool enabled = true;

  // Radius, in patch cells, of the neighbourhood window that turns a similarity
  // into a locally-supported one. 0 turns the filter off, which reproduces the
  // plain ratio test and is what the sweep uses as its control row.
  //
  // 1 is the default: a 3x3 window tolerates a one-cell misalignment, which is
  // what a marker seen at an angle produces, without blurring across more of the
  // marker than the evidence supports.
  int contextRadius = 1;

  // Radius, in frame-patch cells, of the disc around a winner that the runner-up
  // is not allowed to come from. See point 2 in the file comment: without it the
  // runner-up is the neighbouring cell and the ratio test is vacuous.
  //
  // One cell is also the smallest value that makes the runner-up a distinct
  // hypothesis, since neighbours in a square grid include diagonals.
  int nmsRadius = 1;

  // The floor on the *neighbourhood* score. Deliberately low: it exists to throw
  // away patches with no support at all, not to pre-filter, because the voting
  // stage below is what separates signal from noise. See point 3.
  float minSimilarity = 0.45f;

  // Ratio test, on neighbourhood scores, against the runner-up from outside the
  // NMS disc. Applied only when `useRatioTest` is on; with voting on it is an
  // extra, not the main filter.
  float ratioThreshold = 0.90f;
  bool useRatioTest = false;

  // Keep only matches the frame patch also ranks first. Strong on its own, and on
  // a smooth token grid it costs real recall, so it is a refinement rather than a
  // gate.
  bool useMutualCheck = false;

  // A match set this small is noise, and a homography fitted to noise is
  // arbitrary, so require this many surviving correspondences before fitting.
  int minMatches = 15;

  // ... and then this many must agree with the fitted homography, and this
  // fraction of them. Patch tokens from two different images can be genuinely
  // similar over a large area, so the inlier *ratio* matters more here than the
  // absolute count: a background patch field that matches without any geometry is
  // the failure mode to shut down.
  int minInliers = 10;
  float minInlierRatio = 0.45f;

  // Which models are allowed to explain the correspondences.
  //
  // This is not a detail. A full homography has 8 degrees of freedom, fitted here
  // from a 14x14 grid of patch centres that are ~51 pixels apart in the marker's crop
  // -- and 51 pixels is a long way to observe a projective distortion from. The
  // denominator of the fitted transform is then only weakly constrained, and it is
  // free to wander to near-zero: the measured fits put the outline at 29x the frame's
  // area on one frame and 2.1x on the next, both while nominally explaining dozens of
  // patch centres. Those are not bad fits of the marker; they are the projective term
  // fitting noise in a way no image could have produced.
  //
  // The affine model has 6 degrees of freedom, no denominator to wander, and cannot
  // produce a bowtie or a runaway quad at all. For a marker lying flat on a table,
  // filmed by a phone, it is the physically correct model for the great majority of
  // views. Homography is kept available for the views where perspective is real and
  // strong, but it has to earn it: with PreferAffine, the affine model wins unless
  // the homography explains a materially larger share of the support set.
  enum class GeometryModel {
    PreferAffine,  // affine unless the homography is clearly better (default)
    Homography,    // homography alone: the pre-existing behaviour
    Affine         // affine alone: the control row for the sweep
  };
  GeometryModel geometry = GeometryModel::PreferAffine;

  // How much better the homography has to be, as a fraction of the support set,
  // before it displaces the affine model. Zero means "any improvement wins", which
  // on a noisy fit is any improvement at all.
  float homographyMargin = 0.10f;

  // How many deterministic 4-point hypotheses the voting stage tries. 0 falls
  // back to fitting over the filtered set only, which is the pre-voting
  // behaviour and is kept for the sweep's control row.
  //
  // Each hypothesis is scored against every loose correspondence, so this is
  // O(votes x candidates) 3x3 least squares on a 196-point set -- microseconds,
  // and far cheaper than the ORB detector run it replaces.
  int voteHypotheses = 32;

  // Reprojection tolerance for scoring a voting hypothesis, in patch cells. A
  // patch is worth one cell, so one cell of disagreement is already a different
  // patch.
  float voteThresholdCells = 1.0f;

  // The final fit's tolerance, also in patch cells, and deliberately *tighter* than
  // the vote's.
  //
  // The vote only has to rank hypotheses against each other, so a loose tolerance
  // is harmless there -- it is the ratio between competitors that matters. The final
  // fit is different: its tolerance is an assertion about how far a patch centre may
  // land from where the transform says, and one whole cell is most of the marker's
  // width. At 1.0 cell a fold in the outline can satisfy the inlier test, because a
  // neighbouring patch centre is inside the tolerance by definition -- which is
  // exactly how a fit with a hundred inliers ends up producing a bowtie. Half a cell
  // is still forgiving of the marker's own depth and of the sub-pixel drift of a
  // moving view, while being small enough that adjacent patches stop agreeing.
  float refineThresholdCells = 0.5f;

  // RANSAC/MAGSAC confidence and iteration cap for the final fit.
  double confidence = 0.995;
  int maxIters = 2000;

  // Diagnostic escape hatch: if set, the next match() writes the whole
  // correspondence field to this path -- every marker patch, its argmax frame patch,
  // and both scores there -- as CSV, then clears the option so it fires once.
  //
  // This is here because the funnel counters cannot answer the question that
  // decides whether dense localisation is viable at all, which is whether the argmax
  // map is a *coherent* map of the marker onto the frame or merely a smooth drift of
  // a similarity field that is nearly flat everywhere. Those two look identical in
  // `votes_tried` and `vote_best_inliers` and demand opposite responses, so the field
  // itself has to be inspectable. Off by default; nothing in the demo sets it.
  std::string similarityDumpPath;

  // Reject a fit that maps the marker onto something degenerate -- collapsed to a
  // line, folded onto itself, or shrunk to a speck. On by default because a
  // homography can satisfy the inlier test and still be nonsense.
  //
  // These bounds are fractions of the frame, not pixels. An absolute floor is a
  // threshold expressed in whatever resolution happened to be chosen, and it means
  // something different on a 480-wide phone frame than on a 4K one: the 400px^2
  // minimum that seemed generous on a small frame let a 577px^2 speck through on
  // exactly that frame. The two below are the smallest outline worth calling the
  // marker at all -- 0.2% of the frame's area, and corners at least 2% of the frame's
  // width apart.
  bool requirePlausibleQuad = true;
  float minQuadAreaFraction = 0.002f;
  float minQuadSpanFraction = 0.02f;
  float maxQuadAreaFraction = 0.9f;

  // A quad whose corners fold over each other -- a bowtie -- can pass an inlier test
  // on one of its halves and still is not a view of a flat object. Cross products of
  // consecutive edges all carrying the same sign is the test for that, and it costs
  // four subtractions.
  bool requireConvexQuad = true;
};

// What each stage of the correspondence funnel rejected, and what the homography
// then made of what survived.
//
// This exists because the acceptance thresholds above are only meaningful
// relative to the distribution they sit in: "15 matches, 45% inliers" is either
// comfortable or hopeless depending on how many patches there were to begin with
// and what the losing candidates scored. Reporting the funnel is what lets those
// thresholds be chosen from measurement instead of by feel -- the self-test's
// --sweep dumps exactly these numbers for every frame.
struct PatchMatchDiagnostics {
  // The size of the problem: one candidate per marker patch.
  int patches = 0;

  // How many marker patches had *any* best match above minSimilarity. This is the
  // loose set the voting stage consumes.
  int aboveFloor = 0;

  // ... that also passed the ratio test (only when one was applied).
  int afterRatio = 0;

  // ... that were also the frame patch's own best match (only when applied).
  int mutual = 0;

  // Those three counts are `matches`; the rest is the homography's verdict.
  int matches = 0;
  int inliers = 0;
  float inlierRatio = 0.0f;
  bool accepted = false;

  // Why the stage declined, when it did.
  //
  // The funnel counts say how many correspondences existed; they cannot say which
  // gate turned them away. Those are different bugs -- "the vote found no
  // agreement" needs a looser vote, "the fit produced a degenerate outline" needs a
  // worse view of the frame -- and a boolean return throws the distinction away. One
  // enum in the diagnostics, which the pipeline already forwards and the self-test
  // already dumps, keeps it for free.
  enum class Decline {
    None,          // accepted
    Disabled,      // dense matching is off
    NoReference,   // the marker carries no dense reference, or the frame no patches
    BadGeometry,   // an empty crop, or an unusable patch grid
    NoSimilarity,  // the similarity matrix could not be formed
    NoCandidates,  // no marker patch cleared the floor
    TooFewMatches, // some cleared it, fewer than minMatches
    NoVote,        // no hypothesis reached minInliers
    TooFewSupported, // the winner agreed with fewer than four correspondences
    FitFailed,     // the estimator produced nothing usable
    InlierGate,    // too few of the support set's points survived the fit
    DegenerateQuad // the fit disagreed with the inlier test about the outline
  };
  Decline decline = Decline::None;

  // How many of the voting stage's hypotheses were tried, and how many of them
  // supported at least minInliers correspondences. A frame where the winner is
  // the only hypothesis with support is a frame being carried by one lucky guess;
  // a frame where many agree is a frame with a real transform in it.
  int votesTried = 0;
  int votesSupported = 0;

  // The winner's inlier count before MAGSAC++ refines it, and the runner-up
  // hypothesis's. Their gap is the margin the vote actually won by.
  int voteBestInliers = 0;
  int voteRunnerUpInliers = 0;

  // Filled in only when the caller asked for them (see
  // PatchMatcher::similarity), because collecting them costs a sort per frame.
  bool haveDistributions = false;

  // cos(marker patch, its best frame patch), over every marker patch.
  float bestCosMin = 0.0f, bestCosP50 = 0.0f, bestCosMax = 0.0f;
  // cos(runner-up) on the same patches. The gap between these two is what the
  // raw ratio test would measure, so it is the number that says whether the
  // neighbourhood filter was needed.
  float secondCosP50 = 0.0f, secondCosP90 = 0.0f;

  // The same two distributions after the neighbourhood filter, which is what the
  // thresholds actually cut. `ctxGapP50` is the discriminator: it is the median of
  // (best - runner-up) over the marker patches, and it is the direct measure of
  // how far this build's correspondence problem is from the unfiltered one.
  float bestCtxP50 = 0.0f, secondCtxP50 = 0.0f, ctxGapP50 = 0.0f, ctxGapMin = 0.0f;

  // The geometry the fit actually produced, before the plausibility test rejected
  // it: the outline's area in frame pixels, its fraction of the frame, and its
  // shortest corner-to-corner span. "The fit disagreed with the inlier test" is not
  // actionable on its own -- these three say whether the outline collapsed, folded,
  // or merely flew out of frame, which are three different bugs with three different
  // fixes. Zero when the fit never got as far as projecting corners.
  double projectedArea = 0.0;
  double areaFraction = 0.0;
  float minSpan = 0.0f;
  bool plausible = false;
  bool finite = false;
  bool convex = false;
  bool affineFallback = false;

  // Which model explained the frame, and how well each of them did on the same
  // points. Both counts are reported whatever won, because the margin between them
  // is the evidence: a large gap is perspective being real, and a gap of zero or one
  // point is the projective term buying nothing for the instability it costs.
  bool modelHomography = false;
  int homographyInliers = 0;
  int affineInliers = 0;

  double ms = 0.0;
};

class PatchMatcher {
public:
  explicit PatchMatcher(const PatchMatchOptions &options = PatchMatchOptions());

  const PatchMatchOptions &options() const;
  void setOptions(const PatchMatchOptions &options);

  // Locates `reference` -- a marker's stored DenseReference -- inside the frame
  // whose region of interest produced `framePatches`, both having come from the
  // same model and input size. `roi` is that region in frame pixels and
  // `frameSize` the whole frame; the returned corners are in frame pixels too.
  //
  // Fills in matches/inliers/inlierRatio and, on success, corners. As with the
  // ORB path, an ordinary "this is not the marker" is a false return with no
  // error; `error` is set only for something structural.
  //
  // `diagnostics`, when given, receives the correspondence funnel -- see
  // PatchMatchDiagnostics. It is filled on every call, including the ones that
  // return false, because "how close was it" is the interesting question when a
  // frame does not localise.
  bool match(const DenseReference &reference, const PatchGrid &framePatches,
             Rect roi, Size frameSize, VerifyResult &result,
             PatchMatchDiagnostics *diagnostics = nullptr);

  // The raw similarity problem, before any threshold is applied: for every marker
  // patch, its best and runner-up frame patch and their cosines, plus the reverse
  // map the mutual check needs.
  //
  // "Runner-up" means outside the NMS disc around the winner, and both the raw
  // cosine and the neighbourhood score are reported at that pair, because the
  // difference between them is the whole point of the filter.
  //
  // Exposed because the distribution it produces is the measurement the
  // thresholds rest on. Nothing else in the demo uses it.
  struct Similarity {
    int patches = 0;
    int framePatches = 0;
    std::vector<int> bestIndex;
    std::vector<int> secondIndex;
    std::vector<float> bestCos;
    std::vector<float> secondCos;
    std::vector<float> bestCtx;
    std::vector<float> secondCtx;
    std::vector<int> bestOfFrame;

    // The neighbourhood score of every candidate, patches x framePatches, so a
    // caller can cut the problem at any threshold without recomputing it.
    Mat context;

    std::string error;
    bool ok() const { return patches > 0; }
  };

  bool similarity(const DenseReference &reference,
                  const PatchGrid &framePatches, Similarity &out);

  double lastMs() const;

private:
  // The loose candidate set: one correspondence per marker patch whose best
  // neighbourhood score clears the floor. `markerPoints` and `framePoints` are
  // parallel, and `candidates` holds the marker patch indices so the voting seeds
  // can be spread over the marker grid rather than over an arbitrary subset.
  struct Candidates {
    std::vector<Point2f> markerPoints;
    std::vector<Point2f> framePoints;
    std::vector<int> candidates;
  };

  // The marker's grid, the crop it was stretched over, and one cell of that crop in
  // pixels. Every tolerance from here on is expressed in cells of this grid, which
  // is the one space the hypotheses and the candidates already share: a marker cell
  // and a frame cell differ in size whenever the marker covers a different fraction
  // of the two crops.
  struct GridSpace {
    Size grid = Size(0, 0);
    Size2f size;
    double cell = 1.0;
  };

  bool buildCandidates(const DenseReference &reference,
                       const PatchGrid &framePatches, Rect roi,
                       const Similarity &sim, Candidates &out,
                       PatchMatchDiagnostics &diag) const;

  // Deterministic hypothesis generation followed by scoring over every candidate.
  // Fills in diag.votesTried / votesSupported / voteBestInliers /
  // voteRunnerUpInliers and, on success, `h` plus the mask of candidates the winning
  // hypothesis supports. Returns false when no hypothesis reaches minInliers.
  bool vote(const Candidates &candidates, const GridSpace &space, Mat &h,
            std::vector<unsigned char> &support, PatchMatchDiagnostics &diag) const;

  // MAGSAC++ over the winner's support set, then the acceptance test and the RoI
  // corners through the fitted transform.
  bool refine(const DenseReference &reference, const Candidates &candidates,
              const std::vector<unsigned char> &support, const GridSpace &space,
              Rect roi, Size frameSize, VerifyResult &result,
              PatchMatchDiagnostics &diag) const;

  bool project(const DenseReference &reference, const Mat &h, Rect roi,
               Size frameSize, VerifyResult &result,
               PatchMatchDiagnostics &diag) const;

  PatchMatchOptions options_;
  TickMeter timer_;
};

} // namespace dinov3
} // namespace samples
} // namespace cv

#endif // OPENCV_DINOV3_PATCH_MATCHER_HPP
