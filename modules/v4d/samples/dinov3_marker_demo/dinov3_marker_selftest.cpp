// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution is at http://opencv.org/license.html.
//
// The head-less self-test for the DINOv3 Marker Demo.
//
// The window lives entirely in dinov3_marker_demo.cpp. Everything that decides
// *whether* the marker is present and *where* it is lives in three V4D-free
// translation units -- dinov3_embedder.cpp, marker_database.cpp and
// geometric_verifier.cpp -- so this program can link exactly those and replay the
// demo's per-frame decision on a clip without a window, a GL context or a
// display. It is the measurement instrument for the overlay: the demo's outline
// is what the user sees, and this is what makes it reproducible.
//
// What it measures is deliberately the same arithmetic the plan performs:
//
//   1. embed the centre crop of the frame at MarkerParams::roiScale,
//   2. nearest-neighbour cosine search over the database,
//   3. if that passes and geometric verification is on, ask the verifier for the
//      four outline corners.
//
// Step 3 is the interesting one. Recognition can be right while the outline is
// useless -- the embedding answers "is the marker I am looking for in view?",
// while the homography answers "and it is here, at this angle". This program
// prints the outline for every recognised frame and can dump it as CSV, so the
// outline's stability can be scored (see scripts/measure-quad-stability.py)
// instead of judged by eye.
//
// usage: example_v4d_dinov3_marker_selftest [options]
//
//   --assets DIR     root holding models/, images/ and videos/
//                    (default: $V4D_ASSET_PATH, then modules/v4d/assets)
//   --clip NAME      video under <assets>/videos/   (default marker_test.mp4)
//   --marker NAME    image under <assets>/images/   (default harddisk_as_marker.jpeg)
//   --every N        process every Nth frame        (default 10)
//   --quick          every 40th frame
//   --dump FILE      per-frame CSV, outline corners included
//   --threshold F    cosine similarity required     (default 0.62)
//   --roi F          centre-crop fraction           (default 0.80)
//   --orb-only       localise with whole-frame ORB instead of the patch matcher,
//                    giving an independent outline on the same frames to score the
//                    dense one against (see scripts/compare-outlines.py)
//   --verbose        print every frame
//
// Exits 0 when the run completed. The measurement itself is the CSV plus the
// summary; a run that never recognises anything is reported as a failure.

#include "dinov3_embedder.hpp"
#include "marker_database.hpp"
#include "geometric_verifier.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

struct Params {
  float threshold = 0.62f;
  float roiScale = 0.80f;
  int inputSize = 224;
  bool verify = true;
  bool orbOnly = false;
};

// Verifier overrides, so a sweep does not need a rebuild per value. These name the
// stages of the dense pipeline rather than one opaque reprojection distance, because
// the stages fail independently and a sweep that cannot tell them apart teaches
// nothing: patchFloor decides whether there are candidates at all, contextRadius
// decides whether the candidates are the right ones, and voteHypotheses decides
// whether the winner can be told from the runner-up.
struct VerifierTuning {
  float patchFloor = -1.0f;      // negative: keep the verifier's own default
  int contextRadius = -1;
  int nmsRadius = -1;
  int minMatches = -1;
  int voteHypotheses = -1;
  float voteThresholdCells = -1.0f;
  float refineThresholdCells = -1.0f;
  int minInliers = -1;
  float minInlierRatio = -1.0f;
  int homographyMarginPercent = -1;
};

VerifierTuning readTuning() {
  VerifierTuning t;
  if (const char *v = std::getenv("V4D_PATCH_FLOOR"))
    t.patchFloor = std::stof(v);
  if (const char *v = std::getenv("V4D_CONTEXT_RADIUS"))
    t.contextRadius = std::stoi(v);
  if (const char *v = std::getenv("V4D_NMS_RADIUS"))
    t.nmsRadius = std::stoi(v);
  if (const char *v = std::getenv("V4D_MIN_MATCHES"))
    t.minMatches = std::stoi(v);
  if (const char *v = std::getenv("V4D_VOTE_HYPOTHESES"))
    t.voteHypotheses = std::stoi(v);
  if (const char *v = std::getenv("V4D_VOTE_THRESHOLD_CELLS"))
    t.voteThresholdCells = std::stof(v);
  if (const char *v = std::getenv("V4D_REFINE_THRESHOLD_CELLS"))
    t.refineThresholdCells = std::stof(v);
  if (const char *v = std::getenv("V4D_MIN_INLIERS"))
    t.minInliers = std::stoi(v);
  if (const char *v = std::getenv("V4D_MIN_INLIER_RATIO"))
    t.minInlierRatio = std::stof(v);
  if (const char *v = std::getenv("V4D_HOMOGRAPHY_MARGIN"))
    t.homographyMarginPercent = std::stoi(v);
  return t;
}

inline cv::Rect centreCrop(const cv::Size &size, float scale) {
  scale = std::clamp(scale, 0.1f, 1.0f);
  const int w = cvRound(size.width * scale);
  const int h = cvRound(size.height * scale);
  const int x = (size.width - w) / 2;
  const int y = (size.height - h) / 2;
  return cv::Rect(x, y, w, h) & cv::Rect(0, 0, size.width, size.height);
}

std::string findAsset(const std::string &rel, const std::string &root) {
  std::vector<std::string> roots;
  if (!root.empty())
    roots.push_back(root);
  if (const char *env = std::getenv("V4D_ASSET_PATH"))
    roots.emplace_back(env);
  roots.push_back("modules/v4d/assets");
  roots.push_back("../modules/v4d/assets");
  for (const auto &r : roots) {
    const std::filesystem::path p = std::filesystem::path(r) / rel;
    if (std::filesystem::exists(p))
      return p.string();
  }
  return {};
}

// The plan embeds a cv::UMat; this runs on cv::Mat. Same code either way -- the
// embedder and the verifier only read pixels.
cv::UMat toUmat(const cv::Mat &m) { return m.getUMat(cv::ACCESS_READ); }

struct Tally {
  int frames = 0;
  int matched = 0;
  int localised = 0;
  int held = 0;
  int dense = 0;
  int keypoints = 0;
  double embedMs = 0.0;
  double verifyMs = 0.0;
  // Why localisation declined, counted. Without this a run that outlines
  // nothing tells you nothing: "0 outlined" is a symptom, the reason is the
  // diagnosis.
  std::map<std::string, int> rejects;
};

double percentile(std::vector<double> v, double p) {
  if (v.empty())
    return 0.0;
  std::sort(v.begin(), v.end());
  const size_t i = std::min(v.size() - 1,
                            static_cast<size_t>(p * (v.size() - 1)));
  return v[i];
}

// Shoelace area of the outline, in pixels squared.
double quadArea(const std::vector<cv::Point2f> &q) {
  double a = 0.0;
  for (size_t i = 0; i < q.size(); ++i) {
    const cv::Point2f &p0 = q[i];
    const cv::Point2f &p1 = q[(i + 1) % q.size()];
    a += static_cast<double>(p0.x) * p1.y - static_cast<double>(p1.x) * p0.y;
  }
  return std::abs(a) * 0.5;
}

// Longest diagonal / shortest side: 1.0 is a square seen head-on, larger is a
// sliver, very large is a homography that has run away.
double longestDiagonal(const std::vector<cv::Point2f> &q) {
  double diag = 0.0;
  for (size_t i = 0; i < q.size(); ++i)
    for (size_t j = i + 1; j < q.size(); ++j)
      diag = std::max(diag, static_cast<double>(cv::norm(q[i] - q[j])));
  return diag;
}

double elongation(const std::vector<cv::Point2f> &q) {
  if (q.size() != 4)
    return 0.0;
  const double diag = longestDiagonal(q);
  double side = std::numeric_limits<double>::max();
  for (size_t i = 0; i < 4; ++i)
    side = std::min(side, static_cast<double>(cv::norm(q[i] - q[(i + 1) % 4])));
  return side > 1e-9 ? diag / side : 0.0;
}

cv::Point2f quadCentre(const std::vector<cv::Point2f> &q) {
  cv::Point2f c(0.f, 0.f);
  for (const auto &p : q)
    c += p;
  return c * (1.f / static_cast<float>(q.size()));
}

int run(const Params &params, const std::string &model,
        const std::string &markerImage, const std::string &clip, int everyN,
        FILE *dump, bool verbose) {
  cv::setNumThreads(0);

  cv::Ptr<Dinov3Embedder> embedder = cv::makePtr<Dinov3Embedder>();
  Dinov3Embedder::Config cfg;
  cfg.inputSize = params.inputSize;
  embedder->setConfig(cfg);
  std::string error;
  if (!embedder->load(model, error)) {
    std::fprintf(stderr, "cannot load %s: %s\n", model.c_str(), error.c_str());
    return 2;
  }

  // Register the marker photograph the same way the plan registers a crop: embed
  // it, keep the thumbnail, and let the database derive the local features.
  cv::Mat markerMat = cv::imread(markerImage, cv::IMREAD_COLOR);
  if (markerMat.empty()) {
    std::fprintf(stderr, "cannot read %s\n", markerImage.c_str());
    return 2;
  }
  cv::Ptr<MarkerDatabase> db = cv::makePtr<MarkerDatabase>();
  {
    cv::Mat thumb;
    cv::resize(markerMat, thumb, cv::Size(640, 640), 0, 0, cv::INTER_AREA);
    std::vector<float> desc;
    PatchGrid grid;
    if (!embedder->embed(toUmat(thumb), desc, grid, error)) {
      std::fprintf(stderr, "cannot embed the marker: %s\n", error.c_str());
      return 2;
    }
    db->addMarker("harddisk", thumb, desc, grid);
  }
  std::printf("registered '%s' from %s (%dx%d), %d local features\n",
              "harddisk", markerImage.c_str(), markerMat.cols, markerMat.rows,
              [&] {
                MarkerRecord rec;
                db->getMarker(0, rec);
                return static_cast<int>(rec.keypoints.size());
              }());

  cv::Ptr<GeometricVerifier> verifier = cv::makePtr<GeometricVerifier>();
  {
    GeometricVerifier::Config vc;
    const VerifierTuning t = readTuning();
    // Each override is applied only when asked for, so an unset variable leaves the
    // verifier's own default in place. Assigning a sentinel instead would be a quiet
    // way to turn "I did not set this" into "set this to a meaningless value".
    if (t.patchFloor >= 0.0f)
      vc.patchFloor = t.patchFloor;
    if (t.contextRadius >= 0)
      vc.contextRadius = t.contextRadius;
    if (t.nmsRadius >= 0)
      vc.nmsRadius = t.nmsRadius;
    if (t.minMatches >= 0)
      vc.minMatches = t.minMatches;
    if (t.voteHypotheses >= 0)
      vc.voteHypotheses = t.voteHypotheses;
    if (t.voteThresholdCells >= 0.0f)
      vc.voteThresholdCells = t.voteThresholdCells;
    if (t.refineThresholdCells >= 0.0f)
      vc.refineThresholdCells = t.refineThresholdCells;
    if (t.minInliers >= 0)
      vc.minInliers = t.minInliers;
    if (t.minInlierRatio >= 0.0f)
      vc.minInlierRatio = t.minInlierRatio;
    if (t.homographyMarginPercent >= 0)
      vc.homographyMargin = static_cast<float>(t.homographyMarginPercent) / 100.0f;
    vc.forceKeypoints = params.orbOnly;
    verifier->setConfig(vc);
    std::printf(
        "verifier: floor %.2f  context r%d  nms r%d  minMatches %d  vote %d @%.2fc  "
        "refine %.2fc  minInliers %d  minRatio %.2f  H-margin %d%%\n",
        vc.patchFloor, vc.contextRadius, vc.nmsRadius, vc.minMatches, vc.voteHypotheses,
        vc.voteThresholdCells, vc.refineThresholdCells, vc.minInliers, vc.minInlierRatio,
        t.homographyMarginPercent >= 0 ? t.homographyMarginPercent
                                       : static_cast<int>(vc.homographyMargin * 100.0f + 0.5f));
  }

  cv::VideoCapture capture(clip);
  if (!capture.isOpened()) {
    std::fprintf(stderr, "cannot open %s\n", clip.c_str());
    return 2;
  }
  const cv::Size frameSize(
      static_cast<int>(capture.get(cv::CAP_PROP_FRAME_WIDTH)),
      static_cast<int>(capture.get(cv::CAP_PROP_FRAME_HEIGHT)));

  std::printf("clip %s, %dx%d, every %d frame%s\n", clip.c_str(),
              frameSize.width, frameSize.height, everyN,
              everyN == 1 ? "" : "s");

  MarkerRecord record;
  db->getMarker(0, record);

  Tally tally;
  std::vector<double> areas, elongs, dAreas, dCentres, diagDeltas;
  std::vector<cv::Point2f> previous;
  std::string previousName;

  cv::Mat frame;
  int index = 0;
  while (capture.read(frame)) {
    const int frameNo = index++;
    if (everyN > 1 && (frameNo % everyN) != 0)
      continue;
    tally.frames++;

    const cv::Rect roi = centreCrop(frame.size(), params.roiScale);
    const cv::Mat crop = frame(roi).clone();

    std::vector<float> desc;
    PatchGrid grid;
    int64_t t0 = cv::getTickCount();
    const bool embedded = embedder->embed(toUmat(crop), desc, grid, error);
    const int64_t t1 = cv::getTickCount();
    if (!embedded)
      continue;
    tally.embedMs += 1000.0 * (t1 - t0) / cv::getTickFrequency();

    int id = -1;
    float score = 0.f;
    std::string name;
    if (!db->search(desc, params.threshold, id, score, name)) {
      previous.clear();
      continue;
    }
    tally.matched++;
    MarkerRecord matched;
    db->getMarker(id, matched);

    std::vector<cv::Point2f> corners;
    float inlierRatio = 0.f;
    int64_t t2 = cv::getTickCount();
    // Same call the plan makes: the query's patch grid plus the region it came
    // from. The temporal filter lives inside verify(), so consecutive frames here
    // exercise it exactly as they do in the demo.
    const Verification v =
        params.verify
            ? verifier->verify(matched, toUmat(frame), roi, grid, frame.size())
            : Verification{};
    const int64_t t3 = cv::getTickCount();
    corners = v.corners;
    inlierRatio = v.inlierRatio;
    const bool localised = v.ok && corners.size() == 4;
    if (localised) {
      if (v.stale)
        tally.held++;
      if (v.which == Localiser::Dense)
        tally.dense++;
      else if (v.which == Localiser::Keypoints)
        tally.keypoints++;
    } else if (!v.reject.empty()) {
      tally.rejects[v.reject]++;
    }
    tally.verifyMs += 1000.0 * (t3 - t2) / cv::getTickFrequency();

    double area = 0.0, elong = 0.0, dArea = 0.0, dCent = 0.0, dDiag = 0.0;
    if (localised && corners.size() == 4) {
      tally.localised++;
      area = quadArea(corners);
      elong = elongation(corners);
      areas.push_back(area);
      elongs.push_back(elong);

      const cv::Point2f centre = quadCentre(corners);
      const double diag = longestDiagonal(corners);
      if (previous.size() == 4 && name == previousName) {
        const double prevArea = quadArea(previous);
        const double prevDiag = longestDiagonal(previous);
        dArea = (area - prevArea) / std::max(1e-9, prevArea);
        dDiag = (diag - prevDiag) / std::max(1e-9, prevDiag);
        dCent = cv::norm(centre - quadCentre(previous)) /
                std::hypot(frame.cols, frame.rows);
        dAreas.push_back(std::abs(dArea));
        dCentres.push_back(dCent);
        diagDeltas.push_back(std::abs(dDiag));
      }
      previous = corners;
    } else {
      previous.clear();
    }
    previousName = name;

    if (dump) {
      // The funnel goes in alongside the verdict, and so does which localiser
      // answered. "0 of 196 patches cleared the floor" and "190 cleared it and the
      // winner still lost the vote" are different bugs, and a CSV carrying only
      // localised=1 cannot tell them apart. `by` is coded the same way as the
      // sibling's self-test, so scripts/compare-outlines.py reads both.
      const int by = !localised      ? 0
                     : v.which == Localiser::Dense ? 1
                                                   : 2;
      std::fprintf(dump,
                   "%s,%d,%.4f,%d,%.3f,%d,%.1f,%.2f,%.2f,%.2f,%.2f,"
                   "%.1f,%.1f,%d,%d,"
                   "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                   "%d,%d,%d,%d,%d,%d,%.4f,%.4f,%d,%.3f,%s\n",
                   clip.c_str(), frameNo, score, localised ? 1 : 0, inlierRatio, by,
                   area, elong, 100.0 * dArea, 100.0 * dDiag, 100.0 * dCent,
                   1000.0 * (t1 - t0) / cv::getTickFrequency(),
                   1000.0 * (t3 - t2) / cv::getTickFrequency(),
                   matched.thumbnail.cols, matched.thumbnail.rows,
                   corners.size() > 0 ? static_cast<int>(corners[0].x) : 0,
                   corners.size() > 0 ? static_cast<int>(corners[0].y) : 0,
                   corners.size() > 1 ? static_cast<int>(corners[1].x) : 0,
                   corners.size() > 1 ? static_cast<int>(corners[1].y) : 0,
                   corners.size() > 2 ? static_cast<int>(corners[2].x) : 0,
                   corners.size() > 2 ? static_cast<int>(corners[2].y) : 0,
                   corners.size() > 3 ? static_cast<int>(corners[3].x) : 0,
                   corners.size() > 3 ? static_cast<int>(corners[3].y) : 0,
                   v.patches, v.aboveFloor, v.votesTried, v.votesSupported, v.voteBest,
                   v.voteRunnerUp, v.correspondences, v.inliers, v.homographyInliers,
                   v.affineInliers, v.modelHomography ? 1 : 0,
                   v.affineFallback ? 1 : 0,
                   v.areaFraction, v.minSpan,
                   // The ORB columns are filled only when the fallback is what
                   // answered, so a zero here means "this row is not an ORB row" rather
                   // than "ORB found nothing".
                   v.which == Localiser::Keypoints ? v.keypointMatches : 0,
                   v.which == Localiser::Keypoints ? v.inlierRatio : 0.0f,
                   v.reject.c_str());
    }

    if (verbose) {
      std::printf("  frame %5d  score %.3f  %s  inliers %.2f", frameNo, score,
                  localised ? "localised" : "no outline ", inlierRatio);
      if (localised && corners.size() == 4)
        std::printf("  area %8.0f  elong %5.2f  dArea %+6.1f%%  dCent %5.2f%%",
                    area, elong, 100.0 * dArea, 100.0 * dCent);
      std::printf("\n");
    }
  }

  const double frameArea =
      static_cast<double>(frameSize.width) * frameSize.height;
  auto pct = [&](double v) { return 100.0 * v; };
  std::printf("\nframes %d   recognised %d (%.1f%%)   outlined %d\n",
              tally.frames, tally.matched,
              tally.frames ? pct(tally.matched) / tally.frames : 0.0,
              tally.localised);
  if (tally.localised > 0) {
    std::printf("  of those outlines: %d from DINOv3 patches, %d from ORB, "
                "%d carried over\n",
                tally.dense, tally.keypoints, tally.held);
  }
  std::printf("embed %.1f ms/frame   verify %.1f ms/localised\n",
              tally.frames ? tally.embedMs / tally.frames : 0.0,
              tally.localised ? tally.verifyMs / tally.localised : 0.0);
  if (!tally.rejects.empty()) {
    std::printf("localisation declined:\n");
    for (const auto &[reason, count] : tally.rejects)
      std::printf("  %5d  %s\n", count, reason.c_str());
  }
  if (!areas.empty()) {
    std::printf("outline area %% of frame:  med %.2f   p05 %.2f   p95 %.2f\n",
                pct(percentile(areas, 0.5)) / frameArea,
                pct(percentile(areas, 0.05)) / frameArea,
                pct(percentile(areas, 0.95)) / frameArea);
    std::printf("outline elongation:      med %.2f   p95 %.2f   max %.2f\n",
                percentile(elongs, 0.5), percentile(elongs, 0.95),
                *std::max_element(elongs.begin(), elongs.end()));
  }
  if (!dAreas.empty()) {
    std::printf("frame-to-frame outline change over %zu pairs:\n", dAreas.size());
    std::printf("  |dArea|      med %6.2f%%   p95 %6.2f%%   max %6.2f%%\n",
                pct(percentile(dAreas, 0.5)), pct(percentile(dAreas, 0.95)),
                pct(*std::max_element(dAreas.begin(), dAreas.end())));
    std::printf("  |dDiagonal|  med %6.2f%%   p95 %6.2f%%   max %6.2f%%\n",
                pct(percentile(diagDeltas, 0.5)), pct(percentile(diagDeltas, 0.95)),
                pct(*std::max_element(diagDeltas.begin(), diagDeltas.end())));
    std::printf("  |dCentre|    med %6.2f%%   p95 %6.2f%%   max %6.2f%%\n",
                pct(percentile(dCentres, 0.5)), pct(percentile(dCentres, 0.95)),
                pct(*std::max_element(dCentres.begin(), dCentres.end())));
  }

  if (dump)
    std::fprintf(dump,
                 "# summary frames=%d matched=%d outlined=%d\n",
                 tally.frames, tally.matched, tally.localised);

  if (tally.matched == 0) {
    std::fprintf(stderr, "\nFAILED: nothing was recognised on %s\n", clip.c_str());
    return 1;
  }
  return 0;
}

void usage(const char *argv0) {
  std::printf(
      "usage: %s [options]\n"
      "  --assets DIR   root holding models/, images/ and videos/\n"
      "  --clip NAME    video under <assets>/videos/ (default marker_test.mp4)\n"
      "  --marker NAME  image under <assets>/images/ (default "
      "harddisk_as_marker.jpeg)\n"
      "  --every N      process every Nth frame (default 10)\n"
      "  --quick        every 40th frame\n"
      "  --dump FILE    per-frame CSV, outline corners included\n"
      "  --threshold F  cosine similarity required (default 0.62)\n"
      "  --roi F        centre-crop fraction (default 0.80)\n"
      "  --orb-only     localise with whole-frame ORB instead of the patch matcher\n"
      "  --verbose      print every frame\n",
      argv0);
}

} // namespace

int main(int argc, char **argv) {
  std::string root, clipName = "marker_test.mp4",
              markerName = "harddisk_as_marker.jpeg", dumpPath;
  int everyN = 10;
  bool verbose = false;
  Params params;

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
    else if (arg == "--clip")
      clipName = next("--clip");
    else if (arg == "--marker")
      markerName = next("--marker");
    else if (arg == "--every")
      everyN = std::atoi(next("--every").c_str());
    else if (arg == "--quick")
      everyN = 40;
    else if (arg == "--dump")
      dumpPath = next("--dump");
    else if (arg == "--threshold")
      params.threshold = std::stof(next("--threshold"));
    else if (arg == "--roi")
      params.roiScale = std::stof(next("--roi"));
    else if (arg == "--no-verify")
      params.verify = false;
    else if (arg == "--orb-only")
      params.orbOnly = true;
    else if (arg == "--verbose")
      verbose = true;
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
  const std::string markerImage = findAsset("images/" + markerName, root);
  const std::string clip = findAsset("videos/" + clipName, root);
  if (model.empty() || markerImage.empty() || clip.empty()) {
    std::fprintf(stderr,
                 "cannot find the demo's assets.\n"
                 "  model:        %s\n"
                 "  marker image: %s\n"
                 "  clip:         %s\n"
                 "pass --assets <dir>, or set V4D_ASSET_PATH.\n",
                 model.c_str(), markerImage.c_str(), clip.c_str());
    return 2;
  }
  if (!std::filesystem::exists(model + ".data")) {
    std::fprintf(stderr,
                 "%s has no sibling .onnx.data -- the DINOv3 weights live "
                 "outside the graph and must sit next to it.\n",
                 model.c_str());
    return 2;
  }

  FILE *dump = nullptr;
  if (!dumpPath.empty()) {
    dump = std::fopen(dumpPath.c_str(), "w");
    if (dump)
      std::fprintf(dump,
                   "clip,frame,score,localised,inlier_ratio,by,"
                   "area,elongation,d_area_pct,d_diag_pct,d_centre_pct,"
                   "embed_ms,verify_ms,thumb_w,thumb_h,"
                   "x0,y0,x1,y1,x2,y2,x3,y3,"
                   "patches,above_floor,votes_tried,votes_supported,vote_best,"
                   "vote_runner_up,correspondences,inliers,homography_inliers,"
                   "affine_inliers,model_h,affine_fallback,area_fraction,min_span,"
                   "orb_matches,orb_inlier_ratio,reject\n");
    else
      std::fprintf(stderr, "cannot write %s, continuing without it\n",
                   dumpPath.c_str());
  }

  std::printf("DINOv3 Marker Demo self-test\n");
  std::printf("  model  %s\n", model.c_str());
  const int rc = run(params, model, markerImage, clip, everyN, dump, verbose);
  if (dump)
    std::fclose(dump);
  return rc;
}
