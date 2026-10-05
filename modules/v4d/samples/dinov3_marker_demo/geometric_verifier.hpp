#ifndef GEOMETRIC_VERIFIER_HPP
#define GEOMETRIC_VERIFIER_HPP

#include "marker_database.hpp"

#include <opencv2/core.hpp>
#include <opencv2/features.hpp>
#include <opencv2/geometry.hpp>  // findHomography lives here in OpenCV 5

#include <array>
#include <string>
#include <vector>

// Which path produced the outline.
enum class Localiser {
  None,  // no outline
  Dense, // DINOv3 patch tokens -> pose. The default, and the only one that
         // holds up from a distance or at a steep angle.
  Keypoints // ORB on the whole frame -> homography. Fallback for a marker with
            // no stored patch grid.
};

// Which stage of the dense pipeline refused, when it did.
//
// A boolean would throw away the one thing worth knowing when a frame does not
// localise: *why*. "no hypothesis reached consensus" needs a different fix from
// "the fit produced an implausible outline", and they are the same symptom from
// the outside -- no outline.
enum class Decline {
  None,
  NoReference,   // the marker carries no patch grid, or the frame has none
  BadGeometry,   // empty crop
  NoSimilarity,  // the similarity matrix could not be formed
  TooFewMatches, // fewer patches cleared the floor than the estimator needs
  NoVote,        // no hypothesis found consensus
  TooFewSupported, // the winner agreed with fewer than four correspondences
  FitFailed,     // the estimator produced nothing
  InlierGate,    // too little of the support set survived the fit
  DegenerateQuad // the fit disagreed with the inlier test about the outline
};

// One verification attempt, before temporal filtering.
struct Verification {
  bool ok = false;
  // True when the corners are a filtered pose older than the last measurement:
  // real, but not from this frame. The HUD draws these differently so a held
  // outline is never mistaken for a fresh one.
  bool stale = false;
  Localiser which = Localiser::None;
  Decline decline = Decline::None;
  std::vector<cv::Point2f> corners;  // in frame pixels, marker order
  int inliers = 0;
  int correspondences = 0;  // candidate pairs the vote scored
  float inlierRatio = 0.0f;
  // Matches that survived the keypoint ratio test. Reported so a fallback outline can
  // be judged: "ORB found it" and "ORB found it from six matches" are different claims,
  // and only the second one should be trusted as an independent reference.
  int keypointMatches = 0;

  // The correspondence funnel, because the acceptance thresholds only mean
  // something next to the distribution they cut into. "15 matches, 45% inliers"
  // is comfortable or hopeless depending on how many patches there were and how
  // far ahead the winner was.
  int patches = 0;         // candidate patches in the marker
  int aboveFloor = 0;      // ... that cleared the score floor
  int votesTried = 0;      // hypotheses the vote attempted
  int votesSupported = 0;  // ... that reached consensus
  int voteBest = 0;        // winner's support, and the runner-up's -- their gap
  int voteRunnerUp = 0;    // is the margin the vote actually won by

  // Which model explained the frame, and how each did on the same points.
  bool modelHomography = false;
  int homographyInliers = 0;
  int affineInliers = 0;
  bool affineFallback = false;  // the projected corners were unusable, so the
                                // affine part of the fit was used instead

  // The outline the fit produced before the plausibility test rejected it, so
  // "the fit disagreed with the inlier test" is actionable: collapsed, folded,
  // and flew-off-to-infinity are three different bugs.
  double areaFraction = 0.0;
  float minSpan = 0.0f;

  std::string reject;  // one-line reason, for the HUD and the self-test
};

// Locates a registered marker in a frame.
//
// ## Why this is not one findHomography() call
//
// The obvious implementation -- detect ORB keypoints on the frame, detect them
// again on the marker's stored crop, match, estimate -- produces an outline that
// lands on the right marker and then changes shape wildly from frame to frame.
// That is not a tuning problem, it is a correspondence problem:
//
//   * On a hand-held clip the marker moves, scales, rotates and foreshortens.
//     ORB re-finds keypoints on the frame from scratch every frame, so at a
//     distance or a steep angle it simply does not re-find the same points. The
//     match list then mixes a few genuine pairs with a majority of
//     nearest-neighbour noise.
//   * Keeping "the 48 best" of those matches, then handing them to RANSAC, means
//     the minimal set is drawn mostly from the noise. RANSAC returns the
//     hypothesis with the most inliers *within the set it was given*; with the
//     set dominated by outliers the winner changes every frame. Repetitive
//     marker textures make it worse, because several different hypotheses fit
//     almost equally well.
//
// ## The dense path, and the two things that are not obvious
//
// The correspondences come from DINOv3's own patch tokens: 196 per pass, free,
// since the forward pass already ran to decide *whether* the marker is present,
// and scale-free by construction, because a token describes the contents of its
// 16x16 window rather than where that window is. That fixes the ORB problem at
// the root. But taking each marker's nearest frame patch is not enough, and two
// further steps are what make it work:
//
//  1. **Neighbourhood context.** DINOv3 trains a patch token to describe its own
//     window, and adjacent windows overlap by 15/16 of their extent, so the token
//     grid is *smooth*: neighbouring cells are nearly the same vector. Measured on
//     the supplied clip, the best and runner-up cosine for the same marker patch
//     sit about 0.01 apart. A plain Lowe ratio test on that is vacuous -- it
//     measures how much the image changes over 16 pixels, not whether the match is
//     ambiguous -- and it collapses the whole correspondence set to noise. A real
//     correspondence is a *ridge* across both grids, so every similarity is
//     replaced by a binomial-weighted mean over a small window of both grids,
//     which turns the ridge into a peak and leaves an isolated spurious peak as a
//     spike.
//
//  2. **Voting.** With a meaningful runner-up a ratio test becomes possible --
//     but discarding 80% of an already-good set before the estimator sees it
//     destroys the redundancy the estimator needs. So the loose set is kept whole
//     and the transform is chosen by voting: deterministic 4-point hypotheses,
//     spread over the marker in a rotated frame, scored against every candidate.
//     A single outlier cannot decide anything, because it is outvoted.
//
// Two things then keep the outline itself honest:
//
//   * **An estimator that can say no, and a model that has to earn itself.** A
//     homography has 8 degrees of freedom fitted from patch centres tens of pixels
//     apart, so its projective term is weakly constrained and free to wander to
//     near-zero -- producing a perfectly well-formed rectangle drawn a hundred
//     times too large, which passes every shape check. So the 6-DOF affine fit is
//     also computed and the homography is only preferred when it explains
//     materially more of the support set.
//   * **A filter over time.** A rigid marker cannot change its projected shape
//     quickly. Splitting the outline into centroid, scale and unit-scale shape,
//     and smoothing the shape hard while letting position and size follow, turns a
//     per-frame estimate that jitters by degrees into one that does not.
//
// Note what verification is *not*: it is not a vote on whether the marker is
// present. The embedding already decided that, and a failed or filtered
// localisation downgrades the outline, never the recognition.
class GeometricVerifier {
public:
  struct Config {
    // --- dense correspondence -------------------------------------------------
    // Radius, in patch cells, of the neighbourhood window that turns a similarity
    // into a locally-supported one. 1 is the default: a 3x3 window tolerates a
    // one-cell misalignment, which is what an angled marker produces, without
    // smearing across more of the marker than the evidence supports.
    int contextRadius = 1;

    // Radius, in frame-patch cells, of the disc around a winner that the
    // runner-up may not come from. Without it the runner-up *is* the neighbouring
    // cell. One cell is also the smallest value that works, since neighbours in a
    // square grid include diagonals.
    int nmsRadius = 1;

    // Floor on the *neighbourhood* score, not on the raw cosine. Deliberately low:
    // it exists to drop patches with no support at all, not to pre-filter, because
    // the vote is what separates signal from noise.
    float patchFloor = 0.45f;

    // Ratio test on neighbourhood scores against the runner-up from outside the
    // NMS disc. Off by default: it costs real recall on a smooth token grid and
    // the vote already handles the noise it would remove.
    float patchGap = 0.90f;
    bool useRatioTest = false;

    // Keep only matches the frame patch also ranks first. A refinement, not a gate:
    // on a smooth grid it removes genuine matches too.
    bool useMutualCheck = false;

    // A set this small is noise, and a homography fitted to noise is arbitrary.
    int minMatches = 15;

    // How many deterministic 4-point hypotheses the vote tries, and the tolerance
    // it scores them at -- in patch cells of the marker's own crop, because that is
    // the space the hypotheses and the candidates share. Each is scored against
    // every candidate, so this is microseconds.
    int voteHypotheses = 32;
    float voteThresholdCells = 1.0f;

    // The final fit's tolerance, also in cells, and deliberately *tighter* than
    // the vote's. The vote only ranks hypotheses against each other, so a loose
    // tolerance is harmless there; the final fit is an assertion about where a
    // patch centre must land, and at a whole cell a fold in the outline satisfies
    // the inlier test -- a neighbouring patch is inside the tolerance by
    // definition. That is how a fit with a hundred inliers produces a bowtie.
    float refineThresholdCells = 0.5f;

    // Which models may explain the correspondences.
    enum class Geometry {
      PreferAffine,  // affine unless the homography is clearly better (default)
      Homography,
      Affine
    };
    Geometry geometry = Geometry::PreferAffine;

    // How much better the homography must explain, as a fraction of the support
    // set, before it displaces the affine model. Zero means "any improvement
    // wins", which on a noisy fit is any improvement at all.
    float homographyMargin = 0.10f;

    int minInliers = 10;
    float minInlierRatio = 0.45f;
    double confidence = 0.995;
    int maxIterations = 2000;

    // A fit that maps the marker onto something degenerate -- collapsed to a line,
    // folded onto itself, or shrunk to a speck -- is refused, and so is one larger
    // than the frame it was found in.
    //
    // These are fractions of the frame, not pixel counts. An absolute floor is a
    // threshold expressed in whatever resolution happened to be chosen: the 400px^2
    // minimum that seemed generous on a small frame let a 577px^2 speck through on
    // exactly that frame.
    float minQuadAreaFraction = 0.002f;
    float maxQuadAreaFraction = 0.9f;
    float minQuadSpanFraction = 0.02f;
    bool requireConvexQuad = true;

    // --- keypoint fallback ----------------------------------------------------
    // Lowe ratio applied to the Hamming distances. The previous version declared
    // this and never used it, which is why its match list was mostly noise.
    float keypointRatio = 0.78f;
    double keypointReprojection = 3.0;
    int keypointMaxMatches = 256;

    // --- temporal filtering ---------------------------------------------------
    // Exponential blend rate for the unit-scale *shape*. Small means the outline
    // holds still and lags a genuine viewpoint change; large means it tracks
    // closely and jitters.
    float shapeSmoothing = 0.12f;
    // Largest shape change accepted in one frame, as a fraction of the marker's
    // own size. Bounds how far a single bad estimate can pull the shape before the
    // filter has caught up.
    float maxShapeStep = 0.25f;
    // Blend rates for position and size, which genuinely move every frame and so get
    // followed rather than smoothed.
    float centreSmoothing = 0.6f;
    float scaleSmoothing = 0.35f;
    // How many consecutive frames a filtered outline survives without a fresh
    // measurement, so a marker that blinks out for two frames does not strobe.
    int holdFrames = 10;

    // Skip the dense path and localise with keypoints only. Not a demo feature: it
    // exists so the self-test can run a *second, independent* localiser over the same
    // frames and score the dense quad against it. The dense path cannot validate
    // itself -- a collapsed fit and a correct one both report dozens of inliers and
    // both produce a convex, plausible, finite quad.
    bool forceKeypoints = false;
  };

  void setConfig(const Config& cfg);
  const Config& config() const { return cfg_; }

  // `query` is the patch grid of the region of interest in this frame, and `roi`
  // is where that region sits in the frame; the two together place the query's
  // patches in frame coordinates. Pass an empty `query` to fall back to ORB.
  Verification measure(const MarkerRecord& marker, const cv::UMat& frame,
                       const cv::Rect& roi, const PatchGrid& query,
                       const cv::Size& frameSize);

  // measure(), then the temporal filter. Returns the outline to draw, which is the
  // filtered one and may be up to holdFrames old. `corners` is empty only when there
  // is nothing at all to show.
  Verification verify(const MarkerRecord& marker, const cv::UMat& frame,
                      const cv::Rect& roi, const PatchGrid& query,
                      const cv::Size& frameSize);

  // Drops the filter's state. Call when the recognised marker changes or the
  // database is edited: a shape remembered for one marker is meaningless for
  // another.
  void reset();

  // True while the outline being returned is older than the last measurement.
  bool holding() const { return held_ > 0; }
  int heldFrames() const { return held_; }

private:
  // A candidate outline decomposed so that the parts with different dynamics can be
  // filtered separately: where it is (moves every frame), how big it is (moves
  // every frame), and what shape it is (cannot change quickly).
  struct Pose {
    cv::Point2f centre;
    float scale = 0.0f;                  // sqrt(area), pixels
    std::array<cv::Point2f, 4> shape{};  // unit-scale, zero mean, marker order
  };

  // The loose candidate set: one correspondence per marker patch that cleared the
  // floor. `candidates` holds the marker patch indices, so the vote's seeds can be
  // spread over the marker grid rather than over an arbitrary subset.
  struct Candidates {
    std::vector<cv::Point2f> markerPoints;  // marker crop pixels
    std::vector<cv::Point2f> framePoints;   // frame pixels
    std::vector<int> candidates;            // marker patch indices
  };

  // The marker's grid and the crop it was stretched over, plus one cell of that
  // crop in pixels. Every tolerance below is in cells of this grid: a marker cell
  // and a frame cell differ in size whenever the marker covers a different fraction
  // of the two crops, and converting per hypothesis is needless.
  struct GridSpace {
    cv::Size grid;
    cv::Size2f size;
    double cell = 1.0;
  };

  static bool decompose(const std::vector<cv::Point2f>& q, Pose& pose);
  static std::vector<cv::Point2f> compose(const Pose& pose);

  Verification dense(const MarkerRecord& marker, const PatchGrid& query,
                     const cv::Rect& roi, const cv::Size& frameSize,
                     Verification& diag);
  Verification keypoints(const MarkerRecord& marker, const cv::UMat& frame,
                         const cv::Rect& frameRect, const cv::Size& frameSize);

  // `queryGrid` is the shape of the query's patch grid: a frame patch *index* is
  // meaningless without it, since the index only becomes a position once the row
  // width is known.
  bool buildCandidates(const MarkerRecord& marker, const cv::Size queryGrid,
                       const cv::Rect& roi, const std::vector<int>& bestIndex,
                       const std::vector<float>& bestCtx,
                       const std::vector<int>& secondIndex,
                       const std::vector<float>& secondCtx,
                       const std::vector<int>& bestOfFrame, Candidates& out,
                       Verification& diag) const;

  // Deterministic hypothesis generation scored over every candidate. Fills the vote
  // counters and, on success, `h` plus the mask of candidates the winner supports.
  bool vote(const Candidates& candidates, const GridSpace& space, cv::Mat& h,
            std::vector<unsigned char>& support, Verification& diag) const;

  // Fit and score one model over a set of correspondences, at `threshold`. When
  // `support` is given it receives the indices the winning fit kept, which is the
  // one thing that must not be recomputed by fitting a second time: RANSAC is
  // randomised, so a second run disagrees with the first and would seed the affine
  // fit with a different consensus than the homography it is meant to be derived
  // from.
  bool fitAndScore(const std::vector<cv::Point2f>& src,
                   const std::vector<cv::Point2f>& dst, int method, double threshold,
                   cv::Mat& out, int& inliers,
                   std::vector<int>* support = nullptr) const;

  // Robust affine fit, seeded from a homography's inliers so it inherits an
  // already-consensus set rather than rediscovering one.
  bool fitAffineRobust(const std::vector<cv::Point2f>& src,
                       const std::vector<cv::Point2f>& dst, double threshold,
                       const std::vector<int>* seed, cv::Mat& out, int& inliers) const;

  bool project(const MarkerRecord& marker, const cv::Mat& h, const cv::Rect& roi,
               const cv::Size& frameSize, Verification& out,
               Verification& diag) const;

  // The ORB fallback's detector. Owned here so the configuration lives with the code
  // that uses it, and so the demo opens without a display.
  cv::Ptr<cv::Feature2D> detector_ =
      cv::ORB::create(4000, 1.2f, 8, 31, 0, 2, cv::ORB::HARRIS_SCORE, 31, 20);
  Config cfg_;

  // Filter state. One marker at a time: the outline is only meaningful for the
  // marker that produced it.
  int trackedId_ = -1;
  cv::Size frame_;  // the frame the pose's pixel positions are valid for
  Pose pose_;
  bool seeded_ = false;
  int held_ = 0;
};

#endif  // GEOMETRIC_VERIFIER_HPP