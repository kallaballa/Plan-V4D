// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The glue of the DINOv3 Marker Demo: one object that owns the embedder, the
// marker database and the geometric verifier, turns the GUI's request flags into
// database mutations, and fills in the status the HUD and the panel read.
//
// It knows nothing about V4D, which is what lets the head-less self-test drive
// exactly the same code the windowed demo does.
//
// The division of labour between the two signals is the important design point:
//
//   embedding   primary. One DINOv3 descriptor over the region of interest
//               answers "is the marker I am looking for in view?", roughly 45 ms
//               at 224x224. It is deliberately allowed to be wrong about *where*.
//   verification secondary. Local features plus a homography answer "and it is
//               here, at this angle", and are what the HUD outlines. A failed
//               verification therefore downgrades a match to "unconfirmed"; it
//               never throws away an embedding match, because on the measured
//               marker video verification succeeds on about half the frames
//               where the embedding is already confident.
//
// That asymmetry is the reason the demo stays usable on a handheld clip where
// the marker turns and moves in and out of range.

#ifndef OPENCV_DINOV3_MARKER_PIPELINE_HPP
#define OPENCV_DINOV3_MARKER_PIPELINE_HPP

#include "dinov3-embedder.hpp"
#include "dinov3-geometric-verifier.hpp"
#include "dinov3-marker-database.hpp"
#include "dinov3-patch-matcher.hpp"

#include <opencv2/core.hpp>

#include <memory>
#include <string>
#include <vector>

namespace cv {
namespace samples {
namespace dinov3 {

// Everything the GUI writes and the worker reads. The request flags are cleared
// by the worker that consumes them, one-shot, exactly as the playbook's
// takeXRequest() helpers do.
struct MarkerParams {
  bool enabled = true;
  bool hud = true;
  bool drawRoi = true;
  bool verify = true;
  bool autoSave = false;

  // Cosine similarity above which an embedding match counts.
  //
  // 0.50 is measured against the clips shipped in assets/videos rather than
  // guessed. Registering harddisk_as_marker.jpeg and scoring every eighth frame
  // of marker_test.mp4 against four unrelated clips:
  //
  //   threshold   marker_test   unrelated clips (662 frames)
  //   0.42             93.7%      0 false positives
  //   0.50             91.1%      0 false positives
  //   0.60             73.4%      0 false positives
  //   0.70             55.7%      0 false positives
  //
  // The highest score any unrelated frame reaches is 0.406, so 0.50 keeps about
  // 0.09 of headroom. Raising it to 0.60 would roughly double that margin at the
  // cost of twenty points of recall; the slider in the panel is there for
  // exactly that trade on footage this was not tuned on. See README.md.
  float threshold = 0.50f;

  // Centre crop used both for registration and for recognition. The two must
  // agree, or a marker registers as one thing and is looked for as another.
  //
  // 0.90 rather than a tighter crop: the supplied clip shows the marker filling
  // most of the frame, and a tighter crop admits more of the background. Measured
  // against the same clips, 0.90 caught 91.1% where 0.60 caught 82.3%.
  float roiScale = 0.90f;

  int processEveryN = 1;

  // Dense localisation from the embedder's own patch tokens. On by default:
  // measured on the supplied clip it is both far more often right and far
  // cheaper than the keypoint path it replaces as the first attempt.
  bool dense = true;

  char name[64] = "harddisk";

  bool requestRegister = false;
  bool requestUpdateNearest = false;
  bool requestRemove = false;
  bool requestClear = false;
  bool requestSave = false;
  bool requestLoad = false;
};

// Which localiser produced the outline. The demo tries the dense one first
// because it is free, and falls back to whole-frame keypoints when the dense one
// cannot see the marker; the HUD shows which one answered.
enum class LocalisationMode {
  None,
  Dense, // DINOv3 patch tokens -> homography
  Orb,
  Sift
};

struct MarkerStatus {
  bool modelLoaded = false;
  std::string modelError;
  std::string modelDesc;
  int dim = 0;

  size_t markerCount = 0;
  std::string lastMessage;

  // Recognition, for the HUD and the panel.
  bool valid = false;    // an embedding match cleared the threshold
  bool verified = false; // ... and the geometric check agreed
  std::string name;
  float score = 0.0f;
  int matches = 0;
  int inliers = 0;
  float inlierRatio = 0.0f;
  std::vector<Point2f> corners;
  LocalisationMode localisedBy = LocalisationMode::None;

  // True when a dense attempt ran and declined, so the pipeline knows the ORB
  // fallback is still worth its cost.
  bool denseTried = false;
  bool denseRejected = false;

  // The correspondence funnel from each localisation attempt, when one ran.
  // The self-test dumps these per frame: acceptance thresholds are only
  // interpretable next to how many candidates there were and how each filter
  // trimmed them. See PatchMatchDiagnostics.
  PatchMatchDiagnostics dense;
  PatchMatchDiagnostics orbDense; // the ORB path's own funnel, when it matched

  // The region currently embedded, in frame pixels.
  Rect roi;

  // Size of the frame the recognition was made on. The HUD needs this to place
  // `roi` and `corners` on screen: V4D's source stretches the video to the
  // viewport rather than letterboxing it, so the frame-to-screen mapping is a
  // plain scale with no offset.
  Size frameSize;

  double embedMs = 0.0;
  double searchMs = 0.0;
  double verifyMs = 0.0;
  double totalMs = 0.0;
};

// The centre crop both registration and recognition use.
Rect centreCrop(const Size &size, float scale);

struct PipelineOptions {
  EmbedderOptions embedder;
  VerifierOptions verifier;
  PatchMatchOptions patchMatch;
  std::string databasePath = "dinov3_markers.yml.gz";
};

class MarkerPipeline {
public:
  explicit MarkerPipeline(const PipelineOptions &options = PipelineOptions());
  ~MarkerPipeline();

  MarkerPipeline(const MarkerPipeline &) = delete;
  MarkerPipeline &operator=(const MarkerPipeline &) = delete;

  // Loads the model and runs the warm-up pass. On failure modelLoaded stays
  // false, modelError explains why, and the object stays usable -- the demo is
  // required to open without a model.
  bool initModel(MarkerStatus &status);

  // Registers `markerName` from the centre crop of `frame`, computing the
  // embedding and the local features from that same crop. Fails if the name is
  // already taken.
  bool registerMarker(const Mat &frame, const MarkerParams &params,
                      MarkerStatus &status);

  // The same, but the marker does not have to be a frame: this is how the demo
  // starts from a photograph of the marker on disk.
  bool registerMarkerFromImage(const std::string &path, const std::string &name,
                               const MarkerParams &params,
                               MarkerStatus &status);

  // Adds the photograph as `name`, or replaces the record already called `name`.
  // This is what --marker-image uses: re-running the demo with an edited marker
  // image should refresh the marker, not fail because it exists.
  bool upsertMarkerFromImage(const std::string &path, const std::string &name,
                             const MarkerParams &params, MarkerStatus &status);

  // Replaces the nearest existing marker instead of adding one. Refuses when
  // the best match is weaker than max(0.25, 0.75 * threshold), so a moment of
  // low confidence cannot silently overwrite the wrong marker.
  bool updateNearest(const Mat &frame, const MarkerParams &params,
                     MarkerStatus &status);
  bool removeNearest(const Mat &frame, const MarkerParams &params,
                     MarkerStatus &status);

  bool clearDatabase(MarkerStatus &status);
  bool saveDatabase(MarkerStatus &status);
  bool loadDatabase(MarkerStatus &status);

  // Embeds the centre crop of `frame`, searches, and -- when params.verify is
  // on and the embedder's database entry has features -- confirms the match and
  // fills in status.corners.
  bool recognize(const Mat &frame, const MarkerParams &params,
                 MarkerStatus &status);

  // Clears the per-frame recognition fields but keeps the model and database
  // state, so a throttled run does not blank the HUD on skipped frames.
  static void resetRecognition(MarkerStatus &status);

  MarkerDatabase &database();
  Dinov3Embedder *embedder();
  const VerifierOptions &verifierOptions() const;
  const PatchMatchOptions &patchMatchOptions() const;

// Retunes on a live pipeline. The demo puts these on sliders and the self-test
  // sweeps them, because the useful values depend on the footage; see
  // PatchMatchOptions for what each threshold is defending against.
  void setVerifierOptions(const VerifierOptions &options);
  void setPatchMatchOptions(const PatchMatchOptions &options);

  // Where save()/load() read and write. The self-test points this at an
  // unwritable path to prove a failed save is reported and not silently ignored.
  const std::string &databasePath() const;

  // One line per registered marker, for the panel's list.
  std::vector<std::string> markerNames() const;

private:
  // Embeds the centre crop and returns its patch tokens. Every signal is derived
  // from that one rectangle, which is what keeps registration and recognition
  // from describing two different things.
  //
  // Local features are *not* extracted here. ORB detection on the crop costs
  // about as much as the embedding itself, and the dense path usually answers
  // first, so the keypoints are only computed if they are actually needed --
  // either to store a new marker, or when dense matching declines.
  bool describe(const Mat &frame, const MarkerParams &params, Mat &crop,
                Mat &embedding, PatchGrid &patches, LocalFeatures *features,
                Size2f &space, MarkerStatus &status);

  // describe() for a marker that is being stored: embedding, patch tokens and
  // local features, all from one crop.
  bool describeForStorage(const Mat &frame, const MarkerParams &params,
                          Mat &crop, Mat &embedding, DenseReference &dense,
                          LocalFeatures &features, Size2f &space,
                          MarkerStatus &status);

  // describe() plus a downscaled thumbnail, i.e. everything a MarkerRecord needs
  // to be stored. Shared by all four of add, update, register and upsert so they
  // cannot disagree about what a marker's crop is.
  bool buildRecord(const Mat &frame, const MarkerParams &params,
                   const std::string &name, MarkerRecord &record,
                   MarkerStatus &status);

  // Saves when params.autoSave is on, appending any failure to lastMessage.
  void autoSave(const MarkerParams &params, MarkerStatus &status);

  PipelineOptions options_;
  std::unique_ptr<Dinov3Embedder> embedder_;
  std::unique_ptr<MarkerDatabase> db_;
  std::unique_ptr<GeometricVerifier> verifier_;
  std::unique_ptr<PatchMatcher> patcher_;
  TickMeter timer_;
};

} // namespace dinov3
} // namespace samples
} // namespace cv

#endif // OPENCV_DINOV3_MARKER_PIPELINE_HPP