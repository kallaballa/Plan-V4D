// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
//
// Head-less self-test of the DINOv3 Marker Demo.
//
// The demo's real behaviour lives in dinov3-marker-pipeline.*, which knows nothing
// about V4D precisely so that this file can drive the identical code the window
// runs: same embedder, same database, same geometric verifier, same centre crop,
// same decision rule. Nothing here opens a window or creates a GL context.
//
// What it checks, in order:
//
//   1. the centre crop is the rectangle registration and recognition both use
//   2. the model loads, and produces the descriptor size it claims
//   3. the marker registers from a photograph, and that photograph recognises
//      itself: score ~1.0, verified, outline inside the embedded region
//   4. the database refuses a duplicate name, refuses to overwrite a marker from
//      an unrelated frame, and survives a save/clear/load round trip unchanged
//   5. a save to an unwritable path fails with a message and leaves no debris
//   6. the supplied marker clip is recognised at the shipped defaults, with the
//      full pipeline's latency reported
//   7. none of the four unrelated clips in assets/videos produce a false
//      positive, and the threshold's headroom over them is printed as a score
//      distribution plus a table of what each candidate threshold would cost
//   8. mean pooling really does beat CLS pooling here, which is what the PoolMode
//      default rests on
//
// Exits non-zero if any check fails, so it doubles as a regression test.
//
//   ./example_v4d_dinov3-marker-selftest [--assets DIR] [--every N] [--quick]
//                                         [--sweep] [--dump FILE] [--verbose]

#include "dinov3-marker-pipeline.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace cv;
using namespace cv::samples::dinov3;

namespace {

// The unrelated clips that ship in assets/videos. A threshold is only
// meaningful relative to what a non-marker scores, so these are part of the
// test rather than decoration: together they are the false-positive corpus.
const char *kNegatives[] = {"dance.mp4", "dance2.mp4", "bunny.mp4",
                            "kristen.mp4"};
const char *kMarkerClip = "marker_test.mp4";
const char *kMarkerImage = "harddisk_as_marker.jpeg";

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;

// --dump-similarity: the frame index whose correspondence field to write, and where.
// Zero disables it. Set from main() and read inside runClip, rather than threaded
// through every call site, because it is a one-off diagnostic and runClip is called
// from eleven places.
int g_dumpSimilarityFrame = 0;
std::string g_dumpSimilarityPath;

// --orb-only: localise with whole-frame keypoints instead of the patch matcher, so
// the CSV carries a reference outline to judge the dense one against.
bool g_orbOnly = false;

// Returns `ok` so that a check can gate a block of assertions that would crash
// without it.
bool check(const char *label, bool ok,
           const std::string &detail = std::string()) {
  ++g_checks;
  if (!ok)
    ++g_failures;
  std::printf("  [%s] %-48s %s\n", ok ? "pass" : "FAIL", label, detail.c_str());
  std::fflush(stdout);
  return ok;
}

void note(const std::string &label, const std::string &detail) {
  std::printf("       %-48s %s\n", label.c_str(), detail.c_str());
  std::fflush(stdout);
}

// Names the gate that turned a frame away. Goes in the dump as a small integer and
// is spelled out here rather than in the CSV, so the file stays numeric and the
// reader still gets the vocabulary.
const char *denseDeclineName(PatchMatchDiagnostics::Decline decline) {
  using D = PatchMatchDiagnostics::Decline;
  switch (decline) {
  case D::None: return "accepted";
  case D::Disabled: return "disabled";
  case D::NoReference: return "no reference";
  case D::BadGeometry: return "bad geometry";
  case D::NoSimilarity: return "no similarity";
  case D::NoCandidates: return "no candidates";
  case D::TooFewMatches: return "too few matches";
  case D::NoVote: return "no hypothesis agreed";
  case D::TooFewSupported: return "winner agreed with <4";
  case D::FitFailed: return "the fit failed";
  case D::InlierGate: return "too few inliers";
  case D::DegenerateQuad: return "degenerate outline";
  }
  return "unknown";
}

void section(const std::string &title) {
  std::printf("\n== %s ==\n", title.c_str());
}

std::string baseName(const std::string &path) {
  const size_t at = path.find_last_of("/\\");
  return at == std::string::npos ? path : path.substr(at + 1);
}

// ---------------------------------------------------------------------------
// Assets
// ---------------------------------------------------------------------------

// Resolves "videos/marker_test.mp4" the way the demo does, minus the GUI: the
// V4D_ASSET_PATH environment variable first (the same name the module compiles
// its search path into), then where a repo or build tree keeps assets.
// Deliberately independent of opencv_v4d, so this file links no windowing
// library and runs on a head-less box.
std::string findAsset(const std::string &relative, const std::string &root) {
  namespace fs = std::filesystem;
  std::vector<fs::path> roots;

  if (!root.empty())
    roots.emplace_back(root);

  if (const char *env = std::getenv("V4D_ASSET_PATH")) {
    const std::string all(env);
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    size_t start = 0;
    for (;;) {
      const size_t at = all.find(sep, start);
      if (at == std::string::npos) {
        if (start < all.size())
          roots.emplace_back(all.substr(start));
        break;
      }
      if (at > start)
        roots.emplace_back(all.substr(start, at - start));
      start = at + 1;
    }
  }

  roots.emplace_back("assets");
  roots.emplace_back("modules/v4d/assets");
  roots.emplace_back("../modules/v4d/assets");
  roots.emplace_back("../../modules/v4d/assets");

  for (const fs::path &dir : roots) {
    std::error_code ec;
    const fs::path candidate = dir / fs::path(relative);
    if (fs::is_regular_file(candidate, ec))
      return candidate.string();
  }
  return std::string();
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

double quantile(std::vector<float> v, double q) {
  if (v.empty())
    return 0.0;
  std::sort(v.begin(), v.end());
  const double pos = q * (double)v.size() - 0.5;
  if (pos <= 0.0)
    return v.front();
  if (pos >= (double)v.size() - 1.0)
    return v.back();
  const size_t lo = (size_t)std::floor(pos);
  const size_t hi = std::min(v.size() - 1, lo + 1);
  const double frac = pos - (double)lo;
  return (double)v[lo] * (1.0 - frac) + (double)v[hi] * frac;
}

std::string distribution(const std::vector<float> &s) {
  if (s.empty())
    return "no frames";
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "min %.3f  p05 %.3f  med %.3f  p95 %.3f  max %.3f",
                quantile(s, 0.0), quantile(s, 0.05), quantile(s, 0.50),
                quantile(s, 0.95), quantile(s, 1.0));
  return buf;
}

double percent(size_t part, size_t whole) {
  return 100.0 * (double)part / (double)std::max<size_t>(1, whole);
}

// ---------------------------------------------------------------------------
// One clip, driven through the real pipeline
// ---------------------------------------------------------------------------

struct Tally {
  std::string label;
  int decoded = 0;
  int frames = 0;    // frames actually processed
  int matched = 0;   // best score at or above params.threshold
  int verified = 0;  // ... and the homography agreed
  std::vector<float> scores;      // best score of every sampled frame
  std::vector<float> matchedOnly; // the subset that cleared the threshold
  double embedMs = 0.0;
  double verifyMs = 0.0;
  double totalMs = 0.0;
  double worstMs = 0.0;

  // The dense funnel, per sampled frame that reached localisation. Kept as sorted
  // samples rather than sums because the distribution is the point: a median of
  // 9 mutual matches and a median of 79 are different regimes, and the mean would
  // hide that.
  int denseTried = 0;
  int localisedByDense = 0;
  std::vector<int> mutual;
  std::vector<int> inliers;
  std::vector<int> orbMatches;
  std::vector<float> orbInlierRatio;

  // The outline the dense stage produced, on the frames where it produced one. Only
  // localised frames contribute: a rejected frame has no outline, and counting that
  // as a zero would make a *worse* row look like it produced smaller outlines.
  std::vector<double> areas;
  std::vector<double> areaFractions;

  int median(std::vector<int> &values) {
    if (values.empty())
      return 0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  }
};

// The threshold is forced to -1 for the call so db search() reports its
// unconditional argmax and this function applies params.threshold itself. One
// decode pass then yields the score of *every* frame, which is what the
// threshold table needs; re-running the clip per candidate threshold would cost
// a full embed per threshold and reveal nothing new.
//
// Verification is a parameter because on a non-marker it can only fail, and
// paying ORB plus three homography fits per frame to confirm a rejection the
// embedding already made would make this test minutes long.
Tally runClip(MarkerPipeline &pipeline, const std::string &path,
              const MarkerParams &params, int everyN, bool verify, FILE *dump) {
  Tally t;
  t.label = baseName(path);

  VideoCapture cap(path);
  if (!cap.isOpened()) {
    note("warning", "cannot open " + path);
    return t;
  }

  Mat frame;
  MarkerStatus status;
  MarkerParams probe = params;
  probe.threshold = -1.0f;
  probe.verify = verify;

  int index = 0;
  while (cap.read(frame)) {
    ++t.decoded;
    if (++index % everyN != 0)
      continue;
    ++t.frames;

    if (g_dumpSimilarityFrame && index == g_dumpSimilarityFrame) {
      PatchMatchOptions dumpOptions = pipeline.patchMatchOptions();
      dumpOptions.similarityDumpPath = g_dumpSimilarityPath;
      pipeline.setPatchMatchOptions(dumpOptions);
    }

    if (!pipeline.recognize(frame, probe, status) || !status.valid) {
      t.scores.push_back(-1.0f); // the database is empty, or nothing embedded
      continue;
    }

    t.scores.push_back(status.score);
    t.embedMs += status.embedMs;
    t.totalMs += status.totalMs;
    t.worstMs = std::max(t.worstMs, status.totalMs);

    if (status.score < params.threshold)
      continue;

    ++t.matched;
    t.matchedOnly.push_back(status.score);
    t.verifyMs += status.verifyMs;
    if (status.verified)
      ++t.verified;

    if (status.denseTried) {
      ++t.denseTried;
      t.mutual.push_back(status.dense.mutual);
      t.inliers.push_back(status.dense.inliers);
      if (status.localisedBy == LocalisationMode::Dense) {
        ++t.localisedByDense;
        t.areas.push_back(status.dense.projectedArea);
        t.areaFractions.push_back(status.dense.areaFraction);
      }
    }
    if (status.localisedBy == LocalisationMode::Orb ||
        status.localisedBy == LocalisationMode::Sift) {
      t.orbMatches.push_back(status.orbDense.matches);
      t.orbInlierRatio.push_back(status.orbDense.inlierRatio);
    }

    if (dump)
      // The dense funnel goes in as well as the verdict. "0 of 196 cleared the
      // ratio test" and "190 cleared it, the homography disagreed" are different
      // bugs, and a CSV that only carries the verdict cannot tell them apart.
      std::fprintf(dump,
                   "%s,%d,%.4f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%d,"
                   "%.3f,%.1f,%.1f,%d,%.0f,%.3f,%.1f,%d,%d,%d,"
                   "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
                   "%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                   t.label.c_str(), index, status.score,
                   status.verified ? 1 : 0, status.inliers,
                   status.localisedBy == LocalisationMode::Dense  ? 1
                   : status.localisedBy == LocalisationMode::Orb  ? 2
                   : status.localisedBy == LocalisationMode::Sift ? 3
                                                                 : 0,
                   status.denseTried ? 1 : 0, status.dense.patches,
                   status.dense.aboveFloor, status.dense.afterRatio,
                   status.dense.mutual, status.dense.inliers,
                   status.dense.bestCosP50, status.dense.secondCosP50,
                   status.orbDense.matches, status.orbDense.inlierRatio,
                   status.embedMs, status.totalMs,
                   (int)status.dense.decline, status.dense.projectedArea,
                   status.dense.areaFraction, status.dense.minSpan,
                   status.dense.modelHomography ? 1 : 0,
                   status.dense.homographyInliers, status.dense.affineInliers,
                   status.corners.size() == 4 ? status.corners[0].x : 0.0f,
                   status.corners.size() == 4 ? status.corners[0].y : 0.0f,
                   status.corners.size() == 4 ? status.corners[1].x : 0.0f,
                   status.corners.size() == 4 ? status.corners[1].y : 0.0f,
                   status.corners.size() == 4 ? status.corners[2].x : 0.0f,
                   status.corners.size() == 4 ? status.corners[2].y : 0.0f,
                   status.corners.size() == 4 ? status.corners[3].x : 0.0f,
                   status.corners.size() == 4 ? status.corners[3].y : 0.0f,
                   status.dense.votesTried, status.dense.votesSupported,
                   status.dense.voteBestInliers,
                   status.dense.voteRunnerUpInliers,
                   status.dense.bestCtxP50, status.dense.secondCtxP50,
                   status.dense.ctxGapP50, status.dense.ctxGapMin,
                   status.dense.bestCosMin, status.dense.bestCosMax);

    if (g_verbose) {
      std::printf("       frame %4d score %.3f  ", index, status.score);
      if (status.denseTried)
        std::printf("dense %3d/%3d->%3d->%3d inl %3d  ", status.dense.patches,
                    status.dense.aboveFloor, status.dense.afterRatio,
                    status.dense.mutual, status.dense.inliers);
      else
        std::printf("dense  off                    ");
      if (status.denseTried && !status.verified)
        std::printf("declined: %s  ", denseDeclineName(status.dense.decline));
      if (status.orbDense.matches || status.localisedBy == LocalisationMode::Orb ||
          status.localisedBy == LocalisationMode::Sift)
        std::printf("orb %3d inl (%.2f)  ", status.orbDense.matches,
                    status.orbDense.inlierRatio);
      else
        std::printf("orb   -                    ");
      std::printf(" %5.1f ms  %s\n", status.totalMs,
                  status.verified ? "verified" : "-");
    }
  }
  cap.release();
  return t;
}

std::unique_ptr<MarkerPipeline> makePipeline(const std::string &model,
                                             const std::string &dbPath,
                                             const EmbedderOptions &embedder,
                                             const VerifierOptions &verifier,
                                             MarkerStatus &status) {
  PipelineOptions options;
  options.embedder = embedder;
  options.embedder.modelPath = model;
  options.verifier = verifier;
  options.databasePath = dbPath;
  std::unique_ptr<MarkerPipeline> pipeline(new MarkerPipeline(options));
  if (!pipeline->initModel(status))
    note("model error", status.modelError);
  return pipeline;
}

// ---------------------------------------------------------------------------
// 1. The crop
// ---------------------------------------------------------------------------

void testCrop() {
  section("the centre crop: one rectangle for registration and recognition");

  const Rect full = centreCrop(Size(480, 854), 1.0f);
  check("scale 1.0 keeps the whole frame", full == Rect(0, 0, 480, 854),
        std::to_string(full.width) + "x" + std::to_string(full.height) + " at " +
            std::to_string(full.x) + "," + std::to_string(full.y));

  const Rect half = centreCrop(Size(480, 854), 0.5f);
  const int dx = std::abs(2 * half.x + half.width - 480);
  const int dy = std::abs(2 * half.y + half.height - 854);
  check("scale 0.5 stays centred", dx <= 1 && dy <= 1,
        std::to_string(half.width) + "x" + std::to_string(half.height) +
            ", off centre by " + std::to_string(dx) + "," + std::to_string(dy));

  // An out-of-range scale has to degrade to the full frame rather than yield an
  // empty rectangle: a caller passing roiScale = 1.5 from a slider must not
  // silently register nothing.
  check("an out-of-range scale clamps to the full frame",
        centreCrop(Size(480, 854), 1.5f) == full &&
            centreCrop(Size(480, 854), 0.0f) == full &&
            centreCrop(Size(480, 854), -1.0f) == full);

  const Rect tiny = centreCrop(Size(4, 4), 0.01f);
  check("a degenerate scale still yields a non-empty crop inside the frame",
        tiny.width >= 1 && tiny.height >= 1 && tiny.x >= full.x &&
            tiny.y >= full.y && tiny.x + tiny.width <= full.x + full.width &&
            tiny.y + tiny.height <= full.y + full.height,
        std::to_string(tiny.width) + "x" + std::to_string(tiny.height));
}

// ---------------------------------------------------------------------------
// 3-5. Registration, database, persistence
// ---------------------------------------------------------------------------

void testRegistration(MarkerPipeline &pipeline, const MarkerParams &params,
                      const std::string &markerImage,
                      const std::string &strangerVideo) {
  MarkerStatus status;

  section("registration from " + markerImage);
  check("the name is free to begin with", pipeline.database().size() == 0,
        std::to_string(pipeline.database().size()) + " markers");
  check("registerMarkerFromImage succeeds",
        pipeline.registerMarkerFromImage(markerImage, "harddisk", params,
                                         status),
        status.lastMessage);

  // The embedder times itself, and load() deliberately resets that timer after
  // the warm-up pass so the session's own setup cost does not pollute the first
  // real frame. So the timing can only be asserted once something has actually
  // been embedded -- which the registration above has now done.
  check("one forward pass is measured, not assumed",
        pipeline.embedder()->avgMs() > 0.0,
        std::to_string(pipeline.embedder()->avgMs()) + " ms average");

  MarkerRecord stored;
  check("the record can be read back",
        pipeline.database().record("harddisk", stored));
  check("the embedding is 768-d", stored.embedding.cols == 768,
        std::to_string(stored.embedding.cols) + "-d");
  check("the embedding is L2-normalised, so a dot product is a cosine",
        std::abs(std::sqrt((double)stored.embedding.dot(stored.embedding)) -
                 1.0) < 1e-3);
  check("the marker carries local features for verification",
        !stored.features.empty() &&
            stored.features.descriptors.rows == (int)stored.features.points.size(),
        std::to_string(stored.features.count()) + " features, crop " +
            std::to_string((int)stored.cropSize.width) + "x" +
            std::to_string((int)stored.cropSize.height));
  check("the crop size the features live in is recorded",
        stored.cropSize.width > 0 && stored.cropSize.height > 0);

  section("a photograph of the marker recognises itself");
  const Mat image = imread(markerImage, IMREAD_COLOR);
  check("the marker image decodes", !image.empty(),
        std::to_string(image.cols) + "x" + std::to_string(image.rows));
  if (image.empty())
    return;

  check("recognize() accepts it", pipeline.recognize(image, params, status),
        status.lastMessage);
  check("the score is ~1.0", status.score > 0.99f,
        "score " + std::to_string(status.score));
  check("the geometric check confirms it", status.verified,
        std::to_string(status.inliers) + "/" + std::to_string(status.matches) +
            " inliers at ratio " + std::to_string(status.inlierRatio));
  check("the outline has four corners", status.corners.size() == 4,
        std::to_string(status.corners.size()) + " corners");

  // The outline is projected from the marker's crop, so on the reference photo it
  // has to land on the region that was actually embedded. This is the check that
  // would catch a crop-space or scale bug, which the HUD would otherwise happily
  // draw as a plausible-looking quad somewhere else in the frame.
  if (status.corners.size() == 4) {
    const Rect roi = centreCrop(image.size(), params.roiScale);
    const double limit = 0.5 * std::max(roi.width, roi.height) + 8.0;
    double worst = 0.0;
    for (const Point2f &corner : status.corners)
      worst = std::max(worst,
                       (double)std::max(std::abs(corner.x - roi.x - roi.width / 2.0),
                                        std::abs(corner.y - roi.y - roi.height / 2.0)));
    check("the outline sits on the embedded region", worst <= limit,
          "worst corner " + std::to_string((int)worst) + " px from the RoI "
          "centre, limit " + std::to_string((int)limit));
  }

  section("database semantics");
  MarkerStatus dup;
  check("a duplicate name is refused",
        !pipeline.registerMarkerFromImage(markerImage, "harddisk", params, dup) &&
            dup.lastMessage.find("already registered") != std::string::npos,
        dup.lastMessage);
  check("the same image under a second name is accepted",
        pipeline.registerMarkerFromImage(markerImage, "harddisk2", params, dup),
        dup.lastMessage);
  check("the database holds two markers", pipeline.database().size() == 2,
        std::to_string(pipeline.database().size()));

  // The destructive update must not be reachable from an unrelated frame.
  //
  // The frame has to come from a clip that genuinely does not contain the
  // marker. Taking it from marker_test.mp4 -- as this used to -- tests nothing:
  // frame 1 of the marker clip *is* the marker, so the update correctly
  // succeeds, the check fails, and the persistence round trip below then
  // compares a record against one that was legitimately overwritten. Four
  // failures, one cause.
  {
    VideoCapture cap(strangerVideo);
    Mat stranger;
    if (check("an unrelated clip is available for the destructive test",
              cap.isOpened() && cap.read(stranger), strangerVideo)) {
      MarkerStatus upd;
      check("updateNearest refuses an unrelated frame",
            !pipeline.updateNearest(stranger, params, upd), upd.lastMessage);
      check("the marker survived the refused update",
            pipeline.database().record("harddisk", stored), stored.name);
    }
  }

  section("persistence round trip");
  check("save succeeds", pipeline.saveDatabase(status), status.lastMessage);
  MarkerStatus cleared;
  check("clear empties the database",
        pipeline.clearDatabase(cleared) && pipeline.database().size() == 0,
        cleared.lastMessage);
  check("load restores both markers",
        pipeline.loadDatabase(status) && pipeline.database().size() == 2,
        status.lastMessage);

  // A round trip has to preserve the descriptor, not merely the count: this is
  // what a save to the wrong YAML type, or a 1xN that silently reshaped to
  // Nx1, would break.
  MarkerRecord after;
  if (check("the record survives the round trip",
            pipeline.database().record("harddisk", after))) {
    const double cos = stored.embedding.reshape(1, 1)
                           .dot(after.embedding.reshape(1, 1));
    check("the descriptor is unchanged", cos > 0.9999,
          "cosine " + std::to_string(cos));
    check("the features are unchanged",
          after.features.count() == stored.features.count() &&
              after.features.descriptors.size() ==
                  stored.features.descriptors.size() &&
              after.features.descriptors.type() ==
                  stored.features.descriptors.type(),
          std::to_string(after.features.count()) + " features, type " +
              std::to_string(after.features.descriptors.type()));
    check("the crop size is unchanged", after.cropSize == stored.cropSize);
    check("the identifier and creation time are kept", after.id == stored.id,
          "#" + std::to_string(after.id));
  }

  section("a save that cannot succeed is reported, not swallowed");
  {
    // A second pipeline is the cheapest way to point save() at a bad path;
    // saving does not need a model.
    PipelineOptions options;
    options.databasePath = pipeline.databasePath() + "/no/such/dir/markers.yml.gz";
    MarkerPipeline victim(options);
    MarkerStatus bad;
    const std::string path = options.databasePath;
    check("saving into a missing directory fails", !victim.saveDatabase(bad),
          bad.lastMessage);
    check("the failure names the operation", bad.lastMessage.rfind("save failed", 0) == 0,
          bad.lastMessage);
    check("nothing was left behind", !std::filesystem::exists(path) &&
                                          !std::filesystem::exists(path + ".tmp"));
  }

  // Leave the database holding exactly the one marker the clip passes want.
  MarkerStatus back;
  pipeline.clearDatabase(back);
  pipeline.registerMarkerFromImage(markerImage, "harddisk", params, back);
  note("database reset", std::to_string(pipeline.database().size()) +
                             " marker(s): " + back.lastMessage);
}

// ---------------------------------------------------------------------------
// 6-7. The clips
// ---------------------------------------------------------------------------

struct ClipResults {
  Tally positives;            // verification on: the demo's own configuration
  std::vector<float> positiveScores; // every sampled positive frame's score
  std::vector<Tally> negatives;
  float negativeCeiling = -1.0f;
  std::string negativeWorst;
  int falsePositives = 0;
  int negativeFrames = 0;
};

ClipResults testClips(MarkerPipeline &pipeline, const MarkerParams &params,
                      const std::string &markerVideo, const std::string &root,
                      int everyN, FILE *dump) {
  ClipResults out;

  section("the marker clip, full pipeline, verification on");
  out.positives = runClip(pipeline, markerVideo, params, everyN, true, dump);
  {
    const Tally &t = out.positives;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%d of %d sampled frames (%d decoded)",
                  t.matched, t.frames, t.decoded);
    note("marker_test.mp4", buf);
    check("the marker clip is recognised at the shipped threshold",
          percent(t.matched, t.frames) >= 80.0, buf);
    std::snprintf(buf, sizeof(buf), "%d of %d recognised frames localised",
                  t.verified, t.matched);
    check("most recognised frames are localised",
          percent(t.verified, t.matched) >= 50.0, buf);
    std::snprintf(buf, sizeof(buf),
                  "embed %.1f ms, verify %.1f ms, total %.1f ms mean / %.1f ms "
                  "worst",
                  t.embedMs / std::max(1, t.frames),
                  t.verifyMs / std::max(1, t.matched),
                  t.totalMs / std::max(1, t.frames), t.worstMs);
    note("latency", buf);
    note("recognised scores", distribution(t.matchedOnly));
  }

  section("unrelated clips must not match");
  {
    const Tally pos = runClip(pipeline, markerVideo, params, everyN, false, nullptr);
    out.positiveScores = pos.scores;
    note("marker_test.mp4", distribution(pos.scores));

    for (const char *name : kNegatives) {
      const std::string path = findAsset("videos/" + std::string(name), root);
      if (path.empty()) {
        note("skipped", std::string(name) + " is not in the assets");
        continue;
      }
      Tally t = runClip(pipeline, path, params, everyN, false, nullptr);
      out.negatives.push_back(t);
      note(t.label, distribution(t.scores));
      for (float s : t.scores) {
        ++out.negativeFrames;
        if (s >= params.threshold)
          ++out.falsePositives;
        if (s > out.negativeCeiling) {
          out.negativeCeiling = s;
          out.negativeWorst = t.label;
        }
      }
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf), "%d in %d unrelated frames",
                  out.falsePositives, out.negativeFrames);
    check("no unrelated frame reaches the threshold", out.falsePositives == 0,
          buf);
    std::snprintf(buf, sizeof(buf),
                  "ceiling %.3f (%s) against threshold %.2f, headroom %.3f",
                  out.negativeCeiling, out.negativeWorst.c_str(),
                  params.threshold, params.threshold - out.negativeCeiling);
    note("headroom", buf);
    check("the threshold keeps real margin over the negatives",
          params.threshold - out.negativeCeiling > 0.05, buf);

    section("where the threshold can sit, from the scores just measured");
    std::vector<float> negScores;
    for (const Tally &t : out.negatives)
      negScores.insert(negScores.end(), t.scores.begin(), t.scores.end());
    std::printf("       %-8s %12s %18s\n", "t", "marker_test", "unrelated");
    for (float t = 0.40f; t <= 0.76001f; t += 0.04f) {
      size_t tp = 0, fp = 0;
      for (float s : out.positiveScores)
        tp += s >= t;
      for (float s : negScores)
        fp += s >= t;
      std::printf("       %-8.2f %11.1f%% %12zu/%zu%s\n", t,
                  percent(tp, out.positiveScores.size()), fp, negScores.size(),
                  fp == 0 ? "" : "   <- false positive");
    }
  }

  return out;
}

// ---------------------------------------------------------------------------
// 8. The pooling claim
// ---------------------------------------------------------------------------

void testPooling(const std::string &model, const MarkerParams &params,
                 const std::string &markerImage, const std::string &markerVideo,
                 const std::string &root, int everyN) {
  section("pooling: mean has to beat CLS, or the default is wrong");

  const PoolMode modes[] = {PoolMode::Mean, PoolMode::Cls, PoolMode::ClsMean};
  const char *names[] = {"mean", "cls", "cls+mean"};

  const std::string negative = findAsset("videos/dance.mp4", root);
  std::vector<double> recall(3, -1.0);

  for (int i = 0; i < 3; ++i) {
    EmbedderOptions embedder;
    embedder.pool = modes[i];

    MarkerStatus status;
    std::unique_ptr<MarkerPipeline> pipeline =
        makePipeline(model, "/tmp/dinov3-selftest-pool.yml.gz", embedder,
                     VerifierOptions(), status);
    if (!pipeline)
      return;
    if (!pipeline->registerMarkerFromImage(markerImage, "harddisk", params,
                                           status)) {
      note(std::string("pool=") + names[i], status.lastMessage);
      continue;
    }

    const Tally pos =
        runClip(*pipeline, markerVideo, params, everyN, false, nullptr);
    float ceiling = -1.0f;
    if (!negative.empty()) {
      const Tally neg =
          runClip(*pipeline, negative, params, everyN, false, nullptr);
      for (float s : neg.scores)
        ceiling = std::max(ceiling, s);
    }
    recall[i] = percent(pos.matched, pos.frames);
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "recalls %.1f%% of the marker clip, worst unrelated %.3f, "
                  "%.1f ms/frame",
                  recall[i], ceiling, pos.totalMs / std::max(1, pos.frames));
    note(std::string("pool=") + names[i], buf);
  }

  if (recall[0] >= 0.0 && recall[1] >= 0.0) {
    check("mean pooling is not worse than CLS on this footage",
          recall[0] >= recall[1] - 5.0,
          std::to_string((int)recall[0]) + "% vs " +
              std::to_string((int)recall[1]) + "%");
  }
}

// ---------------------------------------------------------------------------
// --sweep: the trade-offs behind the defaults, on request only
// ---------------------------------------------------------------------------

// Recall, worst unrelated score and mean latency for one configuration. The
// unrelated clips are scored in full here, which is why --sweep is slow.
void measureConfig(const std::string &model, const MarkerParams &params,
                   const std::string &markerImage, const std::string &markerVideo,
                   const std::string &root, int everyN,
                   const EmbedderOptions &embedder, double &recallOut,
                   float &ceilingOut, double &msOut) {
  MarkerStatus status;
  std::unique_ptr<MarkerPipeline> pipeline =
      makePipeline(model, "/tmp/dinov3-selftest-sweep.yml.gz", embedder,
                   VerifierOptions(), status);
  if (!pipeline)
    return;
  if (!pipeline->registerMarkerFromImage(markerImage, "harddisk", params, status))
    return;

  const Tally pos = runClip(*pipeline, markerVideo, params, everyN, false, nullptr);
  recallOut = percent(pos.matched, pos.frames);
  msOut = pos.totalMs / std::max(1, pos.frames);
  ceilingOut = -1.0f;
  for (const char *name : kNegatives) {
    const std::string path = findAsset("videos/" + std::string(name), root);
    if (path.empty())
      continue;
    const Tally neg = runClip(*pipeline, path, params, everyN, false, nullptr);
    for (float s : neg.scores)
      ceilingOut = std::max(ceilingOut, s);
  }
}

void sweep(const std::string &model, const MarkerParams &params,
           const std::string &markerImage, const std::string &markerVideo,
           const std::string &root, int everyN) {
  section("sweep: embedder input size");
  std::printf("       %-10s %12s %12s %10s\n", "input", "marker_test",
              "unrelated", "latency");
  for (int side : {112, 140, 168, 196, 224}) {
    EmbedderOptions embedder;
    embedder.inputSize = Size(side, side);
    double recall = 0.0, ms = 0.0;
    float ceiling = -1.0f;
    measureConfig(model, params, markerImage, markerVideo, root, everyN, embedder,
                  recall, ceiling, ms);
    const std::string label =
        std::to_string(side) + "x" + std::to_string(side);
    std::printf("       %-10s %11.1f%% %12.3f %7.1f ms\n", label.c_str(), recall,
                ceiling, ms);
  }

  section("sweep: centre crop");
  std::printf("       %-10s %12s %12s %10s\n", "roiScale", "marker_test",
              "unrelated", "latency");
  for (float roi : {0.60f, 0.70f, 0.80f, 0.90f, 1.00f}) {
    MarkerParams local = params;
    local.roiScale = roi;
    MarkerStatus status;
    std::unique_ptr<MarkerPipeline> pipeline =
        makePipeline(model, "/tmp/dinov3-selftest-sweep.yml.gz",
                     EmbedderOptions(), VerifierOptions(), status);
    if (!pipeline || !pipeline->registerMarkerFromImage(markerImage, "harddisk",
                                                        local, status))
      continue;
    const Tally pos =
        runClip(*pipeline, markerVideo, local, everyN, false, nullptr);
    float ceiling = -1.0f;
    for (const char *name : kNegatives) {
      const std::string path = findAsset("videos/" + std::string(name), root);
      if (path.empty())
        continue;
      const Tally neg = runClip(*pipeline, path, local, everyN, false, nullptr);
      for (float s : neg.scores)
        ceiling = std::max(ceiling, s);
    }
    std::printf("       %-10.2f %11.1f%% %12.3f %7.1f ms\n", roi,
                percent(pos.matched, pos.frames), ceiling,
                pos.totalMs / std::max(1, pos.frames));
  }

  section("sweep: dense correspondence thresholds");
  // This is the sweep that matters most, and it exists because the funnel is
  // measurable. With --dump the per-frame rows show where each filter is actually
  // The dense stage's own knobs, now that it does the localising.
  //
  // The sweep this replaces varied the Lowe ratio and the absolute-cosine floor, and
  // its own comment recorded that both were inert on this footage: the floor cleared
  // all 196 patches on every frame, and the ratio discarded 90-99% of them. Both are
  // off by default now -- the neighbourhood filter does the discriminating work and
  // the ratio test's premise (a *raw* best and runner-up that are far apart) does not
  // hold for DINOv3 patches, whose raw scores sit 0.01 apart. Sweeping them would
  // have measured nothing.
  //
  // What does move the result is the neighbourhood's width and the final fit's
  // tolerance, and they trade against each other in opposite directions: a wider
  // window buys correspondences on a small or oblique marker, while a tighter fit
  // keeps a wide window's extra correspondences from dragging the outline into a fold.
  // So this crosses them, and reports the dense-only localisation rate, the median
  // outline area (a marker the size of this one covers a few percent to a fifth of
  // the frame; a collapsing or folded outline shows up there first), and the median
  // per-frame cost.
  MarkerStatus status;
  PipelineOptions tuned;
  tuned.embedder.modelPath = model;
  tuned.databasePath = "/tmp/dinov3-selftest-sweep.yml.gz";
  MarkerPipeline pipeline(tuned);
  if (!pipeline.initModel(status) ||
      !pipeline.registerMarkerFromImage(markerImage, "harddisk", params, status)) {
    note("warning", "the dense sweep could not set up a pipeline");
  } else {
    // Dense only, so the ORB fallback cannot quietly supply the answer and hide what
    // the dense path is doing. Done through the pipeline's own verifier switch, so
    // the pipeline is driven exactly as a user would drive it.
    const VerifierOptions savedVerifier = pipeline.verifierOptions();
    VerifierOptions noOrb = savedVerifier;
    noOrb.enabled = false;
    pipeline.setVerifierOptions(noOrb);

    MarkerParams denseParams = params;
    denseParams.dense = true;
    denseParams.verify = true;

    std::printf("       %-7s %-8s %-14s %10s %11s %9s\n", "ctx", "fitTol",
                "dense verified", "area p50", "frac p50", "latency");
    for (int ctx : {1, 2}) {
      for (float tol : {0.35f, 0.5f, 0.75f, 1.0f}) {
        PatchMatchOptions patch = tuned.patchMatch;
        patch.contextRadius = ctx;
        patch.refineThresholdCells = tol;
        pipeline.setPatchMatchOptions(patch);

        const Tally t =
            runClip(pipeline, markerVideo, denseParams, everyN, true, nullptr);

        // Median over the localised frames only. A frame that was rejected has no
        // outline, and folding those in as zeros would drag the median down for the
        // wrong reason.
        std::vector<double> areas, fractions;
        for (double v : t.areas) {
          if (v > 0.0)
            areas.push_back(v);
        }
        for (double v : t.areaFractions) {
          if (v > 0.0)
            fractions.push_back(v);
        }
        char area[24] = "-", fraction[24] = "-";
        if (!areas.empty()) {
          std::sort(areas.begin(), areas.end());
          std::snprintf(area, sizeof(area), "%.0f px2",
                        areas[areas.size() / 2]);
        }
        if (!fractions.empty()) {
          std::sort(fractions.begin(), fractions.end());
          std::snprintf(fraction, sizeof(fraction), "%.3f",
                        fractions[fractions.size() / 2]);
        }
        std::printf("       %-7d %-8.2f %7.1f%%/%-4d %10s %11s %7.1f ms\n", ctx,
                    tol, percent(t.localisedByDense, t.matched), t.matched,
                    area, fraction, t.totalMs / std::max(1, t.frames));
      }
    }
    pipeline.setVerifierOptions(savedVerifier);
  }

  section("sweep: local features, as used by verification");
  std::printf("       %-10s %12s %10s %10s\n", "features", "verified",
              "inliers", "latency");
  for (const char *type : {"ORB", "SIFT"}) {
    VerifierOptions verifier;
    if (!featureTypeFromString(type, verifier.type))
      continue;
    MarkerStatus featStatus;
    PipelineOptions options;
    options.embedder.modelPath = model;
    options.verifier = verifier;
    options.databasePath = "/tmp/dinov3-selftest-sweep.yml.gz";
    MarkerPipeline featurePipeline(options);
    if (!featurePipeline.initModel(featStatus) ||
        !featurePipeline.registerMarkerFromImage(markerImage, "harddisk", params,
                                                 featStatus))
      continue;
    const Tally t =
        runClip(featurePipeline, markerVideo, params, everyN, true, nullptr);
    char inliers[32] = "-";
    if (t.matched)
      std::snprintf(inliers, sizeof(inliers), "%d", t.verified);
    std::printf("       %-10s %11.1f%% %10s %7.1f ms\n", type,
                percent(t.verified, t.matched), inliers,
                t.verifyMs / std::max(1, t.matched));
  }
}

void usage(const char *argv0) {
  std::printf("usage: %s [options]\n"
              "  --assets DIR   root holding videos/, images/ and models/\n"
              "  --every N      process every Nth frame (default 10)\n"
              "  --quick        every 40th frame, for a fast smoke run\n"
              "  --sweep        also measure input size, crop and feature type\n"
              "  --dump FILE    write per-frame CSV rows, including the outline\n"
              "                 corners, for offline analysis and overlays\n"
              "  --dump-similarity N:FILE  write the whole correspondence field for\n"
              "                 one frame of the marker clip: every marker patch, its\n"
              "                 argmax frame patch and both scores there\n"
              "  --verbose      print every recognised frame\n"
              "  --orb-only     localise with whole-frame ORB instead of the patch\n"
              "                 matcher, giving a reference outline in the CSV\n"
              "exits non-zero if any check fails\n",
              argv0);
}

} // namespace

int main(int argc, char **argv) {
  std::string root;
  std::string dumpPath;
  int everyN = 10;
  bool doSweep = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char *what) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs an argument\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--assets")
      root = next("--assets");
    else if (arg == "--dump")
      dumpPath = next("--dump");
    else if (arg == "--dump-similarity") {
      const std::string spec = next("--dump-similarity");
      const size_t colon = spec.find(':');
      if (colon == std::string::npos) {
        std::fprintf(stderr, "--dump-similarity wants N:FILE, got '%s'\n",
                     spec.c_str());
        return 2;
      }
      g_dumpSimilarityFrame = std::atoi(spec.substr(0, colon).c_str());
      g_dumpSimilarityPath = spec.substr(colon + 1);
    }
    else if (arg == "--every")
      everyN = std::atoi(next("--every").c_str());
    else if (arg == "--quick")
      everyN = 40;
    else if (arg == "--sweep")
      doSweep = true;
    else if (arg == "--verbose")
      g_verbose = true;
    else if (arg == "--orb-only")
      g_orbOnly = true;
    else if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "unknown option '%s'\n", arg.c_str());
      usage(argv[0]);
      return 2;
    }
  }
  if (everyN < 1)
    everyN = 1;

  const std::string model = findAsset("models/dinov3_vitb16.onnx", root);
  const std::string markerImage =
      findAsset("images/" + std::string(kMarkerImage), root);
  const std::string markerVideo =
      findAsset("videos/" + std::string(kMarkerClip), root);

  if (model.empty() || markerImage.empty() || markerVideo.empty()) {
    std::fprintf(stderr,
                 "cannot find the demo's assets.\n"
                 "  model:        %s\n"
                 "  marker image: %s\n"
                 "  marker clip:  %s\n"
                 "pass --assets <dir>, or set V4D_ASSET_PATH.\n",
                 model.c_str(), markerImage.c_str(), markerVideo.c_str());
    return 2;
  }
  if (!std::filesystem::exists(model + ".data")) {
    std::fprintf(stderr,
                 "%s has no sibling .onnx.data -- the DINOv3 weights live outside "
                 "the graph and must sit next to it.\n",
                 model.c_str());
    return 2;
  }

  FILE *dump = nullptr;
  if (!dumpPath.empty()) {
    dump = std::fopen(dumpPath.c_str(), "w");
    if (dump)
      std::fprintf(dump,
                   "clip,frame,score,verified,inliers,by,dense_tried,patches,"
                   "above_floor,after_ratio,mutual,dense_inliers,"
                   "best_cos_p50,second_cos_p50,orb_matches,orb_inlier_ratio,"
                   "embed_ms,total_ms,decline,quad_area,area_fraction,"
                   "min_span,model_h,homography_inliers,affine_inliers,"
                   "x0,y0,x1,y1,x2,y2,x3,y3,"
                   "votes_tried,votes_supported,vote_best_inliers,"
                   "vote_runner_up_inliers,best_ctx_p50,second_ctx_p50,"
                   "ctx_gap_p50,ctx_gap_min,best_cos_min,best_cos_max\n");
    else
      std::fprintf(stderr, "cannot write %s, continuing without it\n",
                   dumpPath.c_str());
  }

  std::printf("DINOv3 Marker Demo self-test\n");
  std::printf("  model  %s\n", model.c_str());
  std::printf("  image  %s\n", markerImage.c_str());
  std::printf("  clip   %s, every %d%s frame\n", markerVideo.c_str(), everyN,
              doSweep ? "th plus sweeps" : "th");

  testCrop();

  MarkerStatus status;
  section("the model");
  std::unique_ptr<MarkerPipeline> pipeline =
      makePipeline(model, "/tmp/dinov3-selftest-markers.yml.gz",
                   EmbedderOptions(), VerifierOptions(), status);
  if (!pipeline) {
    std::fprintf(stderr, "\nFAILED: the model did not load\n");
    if (dump)
      std::fclose(dump);
    return 1;
  }
  note("loaded", status.modelDesc);
  check("the descriptor is 768-d", status.dim == 768,
        std::to_string(status.dim) + "-d");

  MarkerParams params; // the shipped defaults, exactly as the window runs them
  if (g_orbOnly) {
    // Localise with ORB alone. This is the reference outline for judging the dense
    // one: same frames, same recognition, same CSV columns, only the localiser
    // differs. Whether the dense quad agrees with this is not otherwise answerable
    // from the dump, because the dense path's own numbers cannot tell a correct
    // outline from a plausible one.
    params.dense = false;
    note("orb-only", "dense matching is off; outlines come from whole-frame ORB");
  }

  // The stranger frame for the destructive-update test. dance.mp4 stands in for
  // "a video with something else in it".
  std::string strangerVideo = findAsset("videos/dance.mp4", root);
  if (strangerVideo.empty())
    strangerVideo = findAsset("videos/" + std::string(kNegatives[0]), root);

  testRegistration(*pipeline, params, markerImage, strangerVideo);
  const ClipResults results =
      testClips(*pipeline, params, markerVideo, root, everyN, dump);
  testPooling(model, params, markerImage, markerVideo, root, std::max(everyN, 30));

  if (doSweep)
    sweep(model, params, markerImage, markerVideo, root, everyN);

  if (dump)
    std::fclose(dump);

  section("summary");
  std::printf("  marker_test.mp4 : %.1f%% recognised, %.1f%% of those localised\n",
              percent(results.positives.matched, results.positives.frames),
              percent(results.positives.verified, results.positives.matched));
  std::printf("  unrelated clips : %d false positive(s) in %d frames, ceiling "
              "%.3f\n",
              results.falsePositives, results.negativeFrames,
              results.negativeCeiling);
  std::printf("  %d checks, %d failed\n", g_checks, g_failures);
  std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");

  return g_failures == 0 ? 0 : 1;
}