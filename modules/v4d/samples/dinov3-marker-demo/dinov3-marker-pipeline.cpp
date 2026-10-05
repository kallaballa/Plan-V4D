// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include "dinov3-marker-pipeline.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace cv {
namespace samples {
namespace dinov3 {

namespace {

// Thumbnails go into the YAML database, so keep them small.
constexpr int kThumbnailDim = 96;

// Guard rails for the destructive "update nearest" button: without them, one
// moment of the marker leaving the frame replaces the wrong record.
//
// The relative factor is 1.0, not the 0.75 this started with, and that is
// measured rather than preferred. The unrelated clips in assets/videos peak at a
// cosine of 0.423 against the harddisk marker -- high, because two photographs
// of the same kind of scene share most of their global statistics. A floor of
// 0.75 * 0.5 = 0.375 therefore sits *below* the unrelated ceiling, which means
// the destructive path was more permissive than plain recognition: a frame that
// recognize() correctly refuses at the 0.5 threshold would be accepted here and
// overwrite the record. Requiring an update to clear the same bar as a match
// costs nothing on genuine marker frames (they score ~0.70) and closes that gap.
constexpr float kUpdateAbsoluteFloor = 0.25f;
constexpr float kUpdateRelativeFloor = 1.0f;

float updateFloor(const MarkerParams &params) {
  return std::max(kUpdateAbsoluteFloor, kUpdateRelativeFloor * params.threshold);
}

std::string shortName(const char *name) {
  const std::string s(name);
  return s.empty() ? std::string("(unnamed)") : s;
}

} // namespace

Rect centreCrop(const Size &size, float scale) {
  if (scale <= 0.0f || scale > 1.0f)
    scale = 1.0f;
  const int w = std::max(1, std::min(size.width, cvRound(size.width * scale)));
  const int h = std::max(1, std::min(size.height, cvRound(size.height * scale)));
  return Rect((size.width - w) / 2, (size.height - h) / 2, w, h);
}

MarkerPipeline::MarkerPipeline(const PipelineOptions &options)
    : options_(options),
      embedder_(new Dinov3Embedder(options_.embedder)),
      db_(new MarkerDatabase()),
      verifier_(new GeometricVerifier(options_.verifier)),
      patcher_(new PatchMatcher(options_.patchMatch)) {}

MarkerPipeline::~MarkerPipeline() = default;

MarkerDatabase &MarkerPipeline::database() { return *db_; }
Dinov3Embedder *MarkerPipeline::embedder() { return embedder_.get(); }
const VerifierOptions &MarkerPipeline::verifierOptions() const {
  return options_.verifier;
}
const PatchMatchOptions &MarkerPipeline::patchMatchOptions() const {
  return options_.patchMatch;
}
void MarkerPipeline::setPatchMatchOptions(const PatchMatchOptions &options) {
  options_.patchMatch = options;
  patcher_->setOptions(options);
}
const std::string &MarkerPipeline::databasePath() const {
  return options_.databasePath;
}
void MarkerPipeline::setVerifierOptions(const VerifierOptions &options) {
  options_.verifier = options;
  verifier_->setOptions(options);
}
std::vector<std::string> MarkerPipeline::markerNames() const {
  return db_->names();
}

bool MarkerPipeline::initModel(MarkerStatus &status) {
  std::string error;
  status.modelLoaded = embedder_->load(error);
  if (!status.modelLoaded) {
    status.modelError = error.empty() ? "could not load the model" : error;
    status.modelDesc.clear();
    status.dim = 0;
    return false;
  }
  status.modelError.clear();
  status.modelDesc = embedder_->describe();
  status.dim = embedder_->dim();
  return true;
}

void MarkerPipeline::resetRecognition(MarkerStatus &status) {
  status.valid = false;
  status.verified = false;
  status.score = 0.0f;
  status.matches = 0;
  status.inliers = 0;
  status.inlierRatio = 0.0f;
  status.corners.clear();
  status.localisedBy = LocalisationMode::None;
  status.denseTried = false;
  status.denseRejected = false;
  status.embedMs = 0.0;
  status.searchMs = 0.0;
  status.verifyMs = 0.0;
  status.totalMs = 0.0;
}

bool MarkerPipeline::describe(const Mat &frame, const MarkerParams &params,
                              Mat &crop, Mat &embedding, PatchGrid &patches,
                              LocalFeatures *features, Size2f &space,
                              MarkerStatus &status) {
  if (!embedder_->ready())
    return false;

  const Mat frameMat = frame;
  if (frameMat.empty())
    return false;

  // One crop, used for the embedding and for everything derived from it.
  // Deriving them from separate rectangles is how a marker ends up registered as
  // one thing and searched for as another.
  const Rect roi = centreCrop(frameMat.size(), params.roiScale);
  crop = frameMat(roi);

  // Deliberately no TickMeter here: recognize() owns the overall timing, and the
  // embedder and each localiser report their own.
  std::string error;
  if (!embedder_->embed(crop, embedding, patches, error))
    return false;
  status.embedMs = embedder_->lastMs();

  if (features && options_.verifier.enabled) {
    if (!verifier_->extract(crop, *features, space, error))
      *features = LocalFeatures();
  }
  return true;
}

bool MarkerPipeline::describeForStorage(const Mat &frame,
                                        const MarkerParams &params, Mat &crop,
                                        Mat &embedding, DenseReference &dense,
                                        LocalFeatures &features, Size2f &space,
                                        MarkerStatus &status) {
  PatchGrid patches;
  if (!describe(frame, params, crop, embedding, patches, &features, space,
                status))
    return false;

  // The RoI size the grid was stretched over: the grid index -> pixel mapping
  // needs it, and it is the marker's crop as opposed to the detector's
  // downscaled working size.
  dense.patches = patches.descriptors.empty() ? Mat() : patches.descriptors;
  dense.grid = patches.grid;
  dense.roiSize = Size2f((float)crop.cols, (float)crop.rows);
  return true;
}

bool MarkerPipeline::buildRecord(const Mat &frame, const MarkerParams &params,
                                 const std::string &name, MarkerRecord &record,
                                 MarkerStatus &status) {
  Mat crop, embedding;
  DenseReference dense;
  LocalFeatures features;
  Size2f space;
  if (!describeForStorage(frame, params, crop, embedding, dense, features,
                          space, status))
    return false;

  record.name = name;
  record.embedding = embedding;
  record.features = std::move(features);
  record.cropSize = space;
  record.dense = std::move(dense);
  resize(crop, record.thumbnail, Size(kThumbnailDim, kThumbnailDim));
  return true;
}

void MarkerPipeline::autoSave(const MarkerParams &params,
                              MarkerStatus &status) {
  if (!params.autoSave)
    return;
  std::string saveError;
  if (!db_->save(options_.databasePath, saveError))
    status.lastMessage += "; auto-save failed: " + saveError;
}

bool MarkerPipeline::registerMarker(const Mat &frame,
                                    const MarkerParams &params,
                                    MarkerStatus &status) {
  MarkerRecord record;
  if (!buildRecord(frame, params, shortName(params.name), record, status)) {
    status.lastMessage = "register failed: no embedding for this frame";
    return false;
  }

  std::string error;
  if (!db_->add(record.name, record.embedding, record.thumbnail,
                record.features, record.cropSize, record.dense, record,
                error)) {
    status.lastMessage = "register failed: " + error;
    return false;
  }

  status.lastMessage = "registered '" + record.name + "' (#" +
                       std::to_string(record.id) + ", " +
                       std::to_string(record.features.count()) +
                       " local features)";
  status.markerCount = db_->size();
  autoSave(params, status);
  return true;
}

bool MarkerPipeline::registerMarkerFromImage(const std::string &path,
                                             const std::string &name,
                                             const MarkerParams &params,
                                             MarkerStatus &status) {
  Mat image = imread(path, IMREAD_COLOR);
  if (image.empty()) {
    status.lastMessage = "could not read the marker image '" + path + "'";
    return false;
  }

  MarkerParams local = params;
  std::snprintf(local.name, sizeof(local.name), "%s", name.c_str());
  if (!registerMarker(image, local, status))
    return false;
  status.lastMessage += " from " + path;
  return true;
}

bool MarkerPipeline::upsertMarkerFromImage(const std::string &path,
                                           const std::string &name,
                                           const MarkerParams &params,
                                           MarkerStatus &status) {
  Mat image = imread(path, IMREAD_COLOR);
  if (image.empty()) {
    status.lastMessage = "could not read the marker image '" + path + "'";
    return false;
  }

  MarkerRecord record;
  if (!buildRecord(image, params, name, record, status)) {
    status.lastMessage = "marker image failed: no embedding";
    return false;
  }

  const std::vector<std::string> existing = db_->names();
  const bool existed = std::find(existing.begin(), existing.end(),
                                 record.name) != existing.end();
  std::string error;
  int id = -1;
  if (existed) {
    if (!db_->update(record.name, record.embedding, record.thumbnail,
                     record.features, record.cropSize, record.dense,
                     UpdateMode::ByName, id, error)) {
      status.lastMessage = "marker image failed: " + error;
      return false;
    }
  } else if (!db_->add(record.name, record.embedding, record.thumbnail,
                       record.features, record.cropSize, record.dense, record,
                       error)) {
    status.lastMessage = "marker image failed: " + error;
    return false;
  }

  status.lastMessage =
      std::string(existed ? "refreshed" : "registered") + " '" + record.name +
      "' (#" + std::to_string(id >= 0 ? id : record.id) + ", " +
      std::to_string(record.features.count()) + " local features) from " + path;
  status.markerCount = db_->size();
  autoSave(params, status);
  return true;
}

bool MarkerPipeline::updateNearest(const Mat &frame, const MarkerParams &params,
                                   MarkerStatus &status) {
  Mat crop, embedding;
  DenseReference dense;
  LocalFeatures features;
  Size2f space;
  if (!describeForStorage(frame, params, crop, embedding, dense, features,
                          space, status)) {
    status.lastMessage = "update failed: no embedding for this frame";
    return false;
  }

  const MatchResult nearest = db_->search(embedding, 0.0f);
  if (!nearest.valid()) {
    status.lastMessage = "update failed: the database is empty";
    return false;
  }
  if (nearest.score < updateFloor(params)) {
    status.lastMessage =
        "update refused: nearest scores " +
        std::to_string(nearest.score) + ", below the " +
        std::to_string(updateFloor(params)) + " floor";
    return false;
  }

  Mat thumb;
  resize(crop, thumb, Size(kThumbnailDim, kThumbnailDim));

  int id = -1;
  std::string error;
  if (!db_->update(shortName(params.name), embedding, thumb, features, space,
                   dense, UpdateMode::Nearest, id, error)) {
    status.lastMessage = "update failed: " + error;
    return false;
  }

  status.lastMessage = "updated the marker nearest to the RoI (#" +
                       std::to_string(id) + ", scored " +
                       std::to_string(nearest.score) + ")";
  status.markerCount = db_->size();
  autoSave(params, status);
  return true;
}

bool MarkerPipeline::removeNearest(const Mat &frame, const MarkerParams &params,
                                   MarkerStatus &status) {
  Mat crop, embedding;
  PatchGrid patches;
  LocalFeatures features;
  Size2f space;
  if (!describe(frame, params, crop, embedding, patches, nullptr, space,
                status)) {
    status.lastMessage = "remove failed: no embedding for this frame";
    return false;
  }

  int id = -1;
  std::string error;
  if (!db_->remove(shortName(params.name), UpdateMode::Nearest, id, error)) {
    status.lastMessage = "remove failed: " + error;
    return false;
  }

  status.lastMessage = "removed marker #" + std::to_string(id);
  status.markerCount = db_->size();
  return true;
}

bool MarkerPipeline::clearDatabase(MarkerStatus &status) {
  std::string error;
  if (!db_->clear(error)) {
    status.lastMessage = "clear failed: " + error;
    return false;
  }
  status.lastMessage = "cleared every marker";
  status.markerCount = 0;
  resetRecognition(status);
  return true;
}

bool MarkerPipeline::saveDatabase(MarkerStatus &status) {
  std::string error;
  if (!db_->save(options_.databasePath, error)) {
    status.lastMessage = "save failed: " + error;
    return false;
  }
  status.lastMessage =
      "saved " + std::to_string(db_->size()) + " marker(s) to " +
      options_.databasePath;
  return true;
}

bool MarkerPipeline::loadDatabase(MarkerStatus &status) {
  std::string error;
  if (!db_->load(options_.databasePath, error)) {
    status.lastMessage = "load failed: " + error;
    return false;
  }
  status.lastMessage =
      "loaded " + std::to_string(db_->size()) + " marker(s) from " +
      options_.databasePath;
  status.markerCount = db_->size();
  return true;
}

bool MarkerPipeline::recognize(const Mat &frame, const MarkerParams &params,
                               MarkerStatus &status) {
  resetRecognition(status);

  // Every early return below has to leave totalMs honest, so route them all
  // through finish() rather than repeating the stop.
  auto finish = [&]() {
    timer_.stop();
    status.totalMs = timer_.getLastTimeMilli();
  };

  const Mat frameMat = frame;
  status.frameSize = frameMat.size();
  if (frameMat.empty()) {
    finish();
    return false;
  }
  if (!embedder_->ready()) {
    status.lastMessage = "no model loaded";
    finish();
    return false;
  }
  if (db_->size() == 0) {
    status.lastMessage = "no markers registered yet";
    finish();
    return false;
  }

  timer_.start();

  Mat crop, embedding;
  PatchGrid patches;
  Size2f space;
  if (!describe(frame, params, crop, embedding, patches, nullptr, space,
                status)) {
    finish();
    return false;
  }

  status.roi = centreCrop(frameMat.size(), params.roiScale);

  // Search is a handful of 1x768 dot products, so it gets its own meter purely
  // so the HUD can show it; it is never the reason a frame is slow.
  TickMeter searchTimer;
  searchTimer.start();
  const MatchResult match = db_->search(embedding, params.threshold);
  searchTimer.stop();
  status.searchMs = searchTimer.getLastTimeMilli();
  if (!match.valid()) {
    finish();
    return false;
  }

  status.valid = true;
  status.score = match.score;
  status.name = match.name;

  // The embedding has answered "which marker"; localisation answers "and where",
  // which is what makes the outline possible. A failure here is a downgrade, not
  // a rejection: the marker is still recognised, it is just not outlined this
  // frame.
  if (!params.verify) {
    finish();
    return true;
  }

  MarkerRecord record;
  if (!db_->record(match.name, record)) {
    finish();
    return true;
  }

  // Dense first, from the tokens this frame's embedding already produced. The
  // patch grid only covers the RoI, which is where the embedding looked, so a
  // marker elsewhere in the frame needs the keypoint path -- but that path costs
  // a detector run, so it is paid for only when the dense one declines.
  if (params.dense && options_.patchMatch.enabled && !record.dense.empty() &&
      !patches.empty()) {
    status.denseTried = true;
    TickMeter denseTimer;
    denseTimer.start();
    VerifyResult dense;
    // Ask for the funnel as well as the verdict. A declined frame is the case
    // worth understanding -- how many of the 196 patches cleared each filter, and
    // what the losing candidates scored -- and this is the only place that
    // information exists.
    const bool localised =
        patcher_->match(record.dense, patches, status.roi, frameMat.size(),
                        dense, &status.dense);
    denseTimer.stop();
    status.denseRejected = !localised;

    if (localised) {
      status.verified = true;
      status.corners = dense.corners;
      status.matches = dense.matches;
      status.inliers = dense.inliers;
      status.inlierRatio = dense.inlierRatio;
      status.localisedBy = LocalisationMode::Dense;
      status.verifyMs = denseTimer.getLastTimeMilli();
      finish();
      return true;
    }
    status.verifyMs += denseTimer.getLastTimeMilli();
  }

  if (options_.verifier.enabled && !record.features.empty()) {
    VerifyResult verification;
    if (verifier_->verify(record, frame, verification)) {
      status.verified = true;
      status.corners = verification.corners;
      status.localisedBy = options_.verifier.type == FeatureType::Sift
                               ? LocalisationMode::Sift
                               : LocalisationMode::Orb;
    }
    status.matches = verification.matches;
    status.inliers = verification.inliers;
    status.inlierRatio = verification.inlierRatio;
    // The keypoint path has no patch funnel to record, but recording the verdict
    // alongside the dense one is what makes "dense vs ORB" a comparison rather
    // than two numbers from different frames.
    status.orbDense.matches = verification.matches;
    status.orbDense.inliers = verification.inliers;
    status.orbDense.inlierRatio = verification.inlierRatio;
    status.orbDense.accepted = status.verified;
    status.orbDense.ms = verification.lastMs;
    status.verifyMs += verification.lastMs;
  }

  finish();
  return true;
}

} // namespace dinov3
} // namespace samples
} // namespace cv