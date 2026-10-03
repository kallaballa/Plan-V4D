// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// Skeletal Tracker Demo — real-time multi-person pose estimation with
// MediaPipe BlazePose (OpenCV Zoo / HuggingFace) drawn as a NanoVG skeleton
// overlay, with an ImGui control panel for confidence thresholds.
//
// Models (Apache 2.0, OpenCV Zoo):
//   person_detection_mediapipe_2023mar.onnx
//   pose_estimation_mediapipe_2023mar.onnx
//   https://github.com/opencv/opencv_zoo/tree/main/models/pose_estimation_mediapipe
//   https://huggingface.co/opencv/pose_estimation_mediapipe
//
// The detector, the pose estimator, the tracking and the shared state live in
// skeletal-tracker-pipeline.hpp, so that tools/skeletal-tracker/ can drive the
// same pipeline without redefining main().

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <opencv2/v4d/v4d.hpp>
#include <string>
#include <utility>
#include <vector>

#include "skeletal-tracker-pipeline.hpp"

using namespace cv;
using namespace cv::v4d;
using namespace cv::v4d::event;
using namespace cv::samples;

// ---------------------------------------------------------------------------
// MediaPipe Pose 33-keypoint skeleton topology.
//
// Two joints are also drawn as an emphasised ring: the nose and the mid-hip,
// the two points the detector itself reasons about.
// ---------------------------------------------------------------------------
static const std::vector<std::pair<int, int>> kPoseSegments = {
    // Torso
    {11, 12},
    {11, 23},
    {23, 24},
    {24, 12},
    // Left arm
    {11, 13},
    {13, 15},
    {15, 17},
    {15, 19},
    {15, 21},
    {17, 19},
    // Right arm
    {12, 14},
    {14, 16},
    {16, 18},
    {16, 20},
    {16, 22},
    {18, 20},
    // Left leg
    {23, 25},
    {25, 27},
    {27, 29},
    {27, 31},
    {29, 31},
    // Right leg
    {24, 26},
    {26, 28},
    {28, 30},
    {28, 32},
    {30, 32},
    // Face
    {0, 1},
    {1, 2},
    {2, 3},
    {3, 7},
    {0, 4},
    {4, 5},
    {5, 6},
    {6, 8},
    {9, 10},
};

// BlazePose keypoint indices the rest of the demo refers to by name.
enum PoseJoint {
  kJointNose = 0,
  kJointLeftHip = 23,
  kJointRightHip = 24,
};

// ---------------------------------------------------------------------------
// V4D plan.
// ---------------------------------------------------------------------------
class SkeletalTrackerPlan : public V4DPlan {
private:
    Event<Keyboard> space_ = E<Keyboard>(Keyboard::PRESS);
    Property<double> fps_ = P<double>(GlobalState::Keys::FPS);
public:
  constexpr static auto UMAT_COPY_TO_ =
      _OLMC_(void, cv::UMat, &cv::UMat::copyTo, cv::OutputArray);

  SkeletalTrackerPlan() { _shared(shared_); }

  void setup() override {
    if (pipeline_.empty()) {
      std::string detModel = cv::samples::findFile(
          "models/pose/person_detection_mediapipe_2023mar.onnx");
      std::string poseModel = cv::samples::findFile(
          "models/pose/pose_estimation_mediapipe_2023mar.onnx");
      if (detModel.empty() || poseModel.empty()) {
        CV_Error(Error::StsError, "Pose models not found. Download from OpenCV "
                                  "Zoo or run `make download-models`");
      }
      pipeline_ = makePtr<MediaPipePosePipeline>(detModel, poseModel);
    }
  }

  void gui() override {
    imgui(
        [](SharedPoseState &s) {
          using namespace ImGui;
          Begin("Skeletal Tracker");
          Checkbox("Enable tracking  [Space]", &s.enabled_);
          Checkbox("Smooth (1 Euro)", &s.smooth_);
          SameLine();
          Checkbox("Boxes", &s.showBoxes_);
          Checkbox("Trails", &s.showTrails_);
          Checkbox("Detector RoI", &s.showDetectorBox_);
          Separator();
          SliderFloat("Person conf", &s.detConf_, 0.1f, 0.9f);
          SliderFloat("Pose conf", &s.poseConf_, 0.1f, 0.9f);
          SliderFloat("Keypoint conf", &s.keypointConf_, 0.05f, 0.95f);
          SliderFloat("Pose RoI enlarge", &s.roiEnlarge_, 1.0f, 2.0f);
          SliderInt("Max persons", &s.maxPersons_, 1, 6);
          SliderInt("Coast frames", &s.maxMissed_, 0, 30);
          Separator();
          Text("Persons detected: %zu", s.persons_.size());
          for (const auto &p : s.persons_) {
            // "coasting" is the state to look at while tuning: it means the
            // skeleton on screen is extrapolated, not measured this frame.
            Text("  #%d  det %.2f%s", p.id, p.score,
                 p.missed > 0 ? "  (coasting)" : "");
          }
          Separator();
          Text("detect %.1f ms   pose %.1f ms   total %.1f ms", s.detectMs_,
               s.poseMs_, s.totalMs_);
          if (Button("Fullscreen"))
            s.fullscreen_ = !s.fullscreen_;
          End();
        },
        RWS(shared_));
  }

  void infer() override {
    set(V4D::Keys::FULLSCREEN, CS(shared_.fullscreen_));
    
    RWS(shared_.frameDt_) = fps_;
    branch(RWS(shared_.enabled_) =
               IF(F(&Keyboard::List::empty, space_), CS(shared_.enabled_),
                  !CS(shared_.enabled_)));
    {
      // RGBA2BGR, not RGBA2RGB: both nets take their blob with swapRB set, so
      // what they need is a Mat in OpenCV's own BGR order -- which is what this
      // produces, and what the `frameBGR` name promises. Handing them RGB bytes
      // swapRB turns into BGR bytes, and a channel-swapped image is close enough
      // for the detector to mostly find the dancer but wrong enough that it also
      // fires a second, spurious box every few seconds: the demo then shows a
      // second skeleton on the same person, which mints a second track, coasts
      // it for maxMissed_ frames and drops it again.
      fb([](const cv::UMat &fb,
            cv::UMat &frameBGR) { cvtColor(fb, frameBGR, cv::COLOR_RGBA2BGR); },
         RW(frameBGR_))
          ->plain(runPipeline, RWS(shared_), R(frameBGR_))
          ->nvg(drawOverlay, size_, frameNo_, CS(shared_));
    }
    endBranch();
  }

private:
  static SharedPoseState shared_;
  Property<Size> size_ = P<Size>(V4D::Keys::SIZE);
  Property<uint64_t> frameNo_ = P<uint64_t>(GlobalState::Keys::FRAME_CNT);
  static Ptr<MediaPipePosePipeline> pipeline_;
  UMat frameBGR_;

  static void runPipeline(SharedPoseState &state, const cv::UMat &frameBGR) {
    // An empty frame means no fresh evidence about anyone, so anything left
    // in the shared state from earlier frames is stale: keeping it would let
    // the overlay draw a skeleton the pipeline no longer has an opinion
    // about, indefinitely, while the source stalls.
    if (frameBGR.empty() || pipeline_.empty()) {
      state.persons_.clear();
      state.detectMs_ = 0.f;
      state.poseMs_ = 0.f;
      state.totalMs_ = 0.f;
      return;
    }
    
        
        
        
        
        
        MediaPipePosePipeline::Stats stats;
    state.persons_ = pipeline_->run(frameBGR.getMat(cv::ACCESS_READ),
                                    state.params(), &stats);
    state.detectMs_ = stats.detectMs;
    state.poseMs_ = stats.poseMs;
    state.totalMs_ = stats.totalMs;
  }

  // One colour per track id, so two people on screen are never confused for
  // each other. Indexed, so a track keeps its colour for its whole life.
  static cv::Scalar trackColor(int id) {
    // Not constexpr: cv::Scalar's constructor is not a constant expression.
    static const cv::Scalar kPalette[] = {
        cv::Scalar(60, 220, 255),  // amber
        cv::Scalar(255, 200, 60),  // azure
        cv::Scalar(90, 90, 255),   // red
        cv::Scalar(255, 140, 210), // pink
        cv::Scalar(120, 255, 120), // light green
        cv::Scalar(220, 255, 60),  // teal
        cv::Scalar(225, 120, 255), // violet
        cv::Scalar(60, 255, 255),  // yellow
        cv::Scalar(160, 255, 255), // pale yellow
        cv::Scalar(255, 180, 90),  // sky
    };
    const size_t i =
        static_cast<size_t>(id) % (sizeof(kPalette) / sizeof(kPalette[0]));
    return kPalette[i];
  }

  static void strokeRect(const cv::Rect2f &r) {
    using namespace cv::v4d::nvg;
    beginPath();
    rect(r.x, r.y, r.width, r.height);
    stroke();
  }

  static void drawOverlay(const cv::Size &sz, uint64_t frameNo,
                          const SharedPoseState &state) {
    using namespace cv::v4d::nvg;
    char buf[64];
    const auto &persons = state.persons_;
    const float kpConf = state.keypointConf_;

    // Line widths scale with the viewport so the overlay reads the same at
    // 720p and at 4K.
    const float scale = std::max(1.0f, static_cast<float>(sz.height) / 720.f);
    const float boneW = 3.0f * scale;
    const float jointR = 3.5f * scale;
    // A dark stroke of the same path under the coloured one keeps the
    // skeleton readable over a bright frame as well as a dark one.
    const cv::Scalar outline(0, 0, 0, 150);

    lineCap(NVG_ROUND);
    lineJoin(NVG_ROUND);

    for (const auto &person : persons) {
      const auto &kpts = person.keypoints;
      if (kpts.empty())
        continue;
      const cv::Scalar color = trackColor(person.id);
      // A coasting track is one whose skeleton is extrapolated, not
      // measured this frame; fading it makes that readable at a glance.
      const uchar alpha = person.missed > 0 ? 110 : 235;

      // --- Motion trail: where this track has been over the last frames.
      if (state.showTrails_ && person.trail.size() > 1) {
        const size_t n = person.trail.size();
        for (size_t i = 1; i < n; ++i) {
          globalAlpha(static_cast<float>(i) / static_cast<float>(n) * alpha /
                      255.f);
          strokeColor(cv::Scalar(color[0], color[1], color[2], 1));
          strokeWidth(boneW * 0.8f);
          beginPath();
          moveTo(person.trail[i - 1].x, person.trail[i - 1].y);
          lineTo(person.trail[i].x, person.trail[i].y);
          stroke();
        }
        globalAlpha(1.0f);
      }

      // --- Box around the skeleton.
      if (state.showBoxes_) {
        const cv::Rect2f box = skeletonBox(kpts, kpConf);
        if (box.area() > 0) {
          strokeColor(cv::Scalar(color[0], color[1], color[2], alpha));
          strokeWidth(1.5f * scale);
          strokeRect(box);
        }
      }

      // --- The detector's own RoI, for comparison with the skeleton box
      // above: the two differ by design, the detector's box being a
      // near-constant square around the torso.
      if (state.showDetectorBox_ && person.box.area() > 0) {
        strokeColor(cv::Scalar(color[0], color[1], color[2], alpha / 2));
        strokeWidth(1.0f * scale);
        strokeRect(cv::Rect2f(person.box));
      }

      // --- Bones, outlined then coloured.
      for (int pass = 0; pass < 2; ++pass) {
        strokeColor(pass == 0
                        ? outline
                        : cv::Scalar(color[0], color[1], color[2], alpha));
        strokeWidth(pass == 0 ? boneW * 2.2f : boneW);
        for (const auto &[a, b] : kPoseSegments) {
          if (a >= static_cast<int>(kpts.size()) ||
              b >= static_cast<int>(kpts.size()))
            continue;
          if (kpts[a][3] < kpConf || kpts[b][3] < kpConf)
            continue;
          beginPath();
          moveTo(kpts[a][0], kpts[a][1]);
          lineTo(kpts[b][0], kpts[b][1]);
          stroke();
        }
      }

      // --- Joints: a dark disc under a coloured one, nose and mid-hip
      // larger because they are what the detector itself keys on.
      for (int pass = 0; pass < 2; ++pass) {
        fillColor(pass == 0 ? outline
                            : cv::Scalar(color[0], color[1], color[2], alpha));
        for (size_t i = 0; i < kpts.size(); ++i) {
          if (kpts[i][3] < kpConf)
            continue;
          const bool key =
              i == kJointNose ||
              (i == kJointLeftHip && kpts[kJointRightHip][3] >= kpConf) ||
              (i == kJointRightHip && kpts[kJointLeftHip][3] >= kpConf);
          const float r =
              jointR * (pass == 0 ? (key ? 2.1f : 1.6f) : (key ? 1.4f : 1.0f));
          beginPath();
          circle(kpts[i][0], kpts[i][1], r);
          fill();
        }
      }

      // --- Track id above the head, so identity stability is visible.
      const int head =
          kpts[kJointNose][3] >= kpConf ? kJointNose : kJointLeftHip;
      std::snprintf(buf, sizeof(buf), "#%d", person.id);
      const float labelSize = 18.0f * scale;
      fontSize(labelSize);
      fontFace("sans-bold");
      textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
      const float tx = kpts[head][0];
      const float ty = kpts[head][1] - jointR * 3.0f;
      // Legibility: a filled pill behind the text, in the track colour.
      float bounds[4];
      textBounds(tx, ty, buf, buf + std::strlen(buf), bounds);
      fillColor(cv::Scalar(color[0], color[1], color[2], alpha));
      beginPath();
      roundedRect(bounds[0] - 5 * scale, bounds[1] - 2 * scale,
                  (bounds[2] - bounds[0]) + 10 * scale,
                  (bounds[3] - bounds[1]) + 4 * scale, 4 * scale);
      fill();
      fillColor(cv::Scalar(0, 0, 0, 255));
      text(tx, ty, buf, buf + std::strlen(buf));
    }

    // --- Status line.
    fontSize(18.0f * scale);
    fontFace("sans-bold");
    fillColor(cv::Scalar(255, 255, 255, 210));
    textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    std::snprintf(buf, sizeof(buf), "frame %llu   persons %zu   %.1f ms",
                  static_cast<unsigned long long>(frameNo), persons.size(),
                  state.totalMs_);
    text(12.0f, 12.0f, buf, buf + std::strlen(buf));
  }
};

SharedPoseState SkeletalTrackerPlan::shared_;
cv::Ptr<MediaPipePosePipeline> SkeletalTrackerPlan::pipeline_;

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
  cv::v4d::add_asset_search_paths();

  std::string inputVideo =
      (argc > 1) ? argv[1] : cv::samples::findFile("videos/dance.mp4");
  std::string outputVideo = (argc > 2) ? argv[2] : "";

  // A readable file must exist even when an argument was supplied: without
  // this check a typo in the path just opens an empty window.
  if (inputVideo.empty() || !std::ifstream(inputVideo).good()) {
    std::cerr << "Cannot read input video: "
              << (inputVideo.empty() ? "<no bundled video found>" : inputVideo)
              << "\n"
              << "Usage: skeletal-tracker-demo [input-video] [output-video]"
              << std::endl;
    return 1;
  }

  cv::Rect viewport(0, 0, 1920, 1080);
  cv::Ptr<V4D> runtime = V4D::init(viewport, "Skeletal Tracker",
                                   AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                                   ConfigFlags::DISPLAY_MODE);

  auto src = Source::make(runtime, inputVideo);

  runtime->setSource(src);
  if (!outputVideo.empty()) {
    auto sink = Sink::make(runtime, outputVideo, 60, viewport.size());
    runtime->setSink(sink);
  }

  V4DPlan::run<SkeletalTrackerPlan>(7);
  return 0;
}
