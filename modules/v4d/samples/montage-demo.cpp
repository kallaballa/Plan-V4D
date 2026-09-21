// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

int v4d_cube_main();
int v4d_many_cubes_main();
int v4d_video_main(int argc, char **argv);
int v4d_nanovg_main(int argc, char **argv);
int v4d_shader_main(int argc, char **argv);
int v4d_font_main();
int v4d_pedestrian_main(int argc, char **argv);
int v4d_optflow_main(int argc, char **argv);
int v4d_beauty_main(int argc, char **argv);
#define main v4d_cube_main
#include "cube-demo.cpp"
#undef main
#define main v4d_many_cubes_main
#include "many_cubes-demo.cpp"
#undef main
#define main v4d_video_main
#include "video-demo.cpp"
#undef main
#define main v4d_nanovg_main
#include "nanovg-demo.cpp"
#undef main
#define main v4d_shader_main
#include "shader-demo.cpp"
#undef main
#define main v4d_font_main
#include "font-demo.cpp"
#undef main
#define main v4d_pedestrian_main
#include "pedestrian-demo.cpp"
#undef main
#define main v4d_optflow_main
#include "optflow-demo.cpp"
#undef main
#define main v4d_beauty_main
#include "beauty-demo.cpp"
#undef main

static void drawCell(const cv::Rect &vp) {
  using namespace cv::v4d::nvg;
  beginPath();
  strokeWidth(8);
  strokeColor(cv::Scalar(255, 100, 150, 255));
  rect(8, 8, vp.width - 16, vp.height - 16);
  stroke();
}

static void drawLabel(const cv::Rect &vp, const string &label) {
  using namespace cv::v4d::nvg;
  beginPath();
  strokeWidth(8);
  strokeColor(cv::Scalar(100, 100, 100, 255));
  rect(8, 8, vp.width - 16, vp.height - 16);
  stroke();
  fontSize(120.0f);
  fontFace("sans-bold");
  fillColor(cv::Scalar(100, 100, 100, 255));
  textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
  text(vp.size().width / 2.0, vp.size().height / 2.0 - 30.0, label.c_str(),
       label.c_str() + label.size());
}

class BlankPlan : public V4DPlan {
public:
  void infer() override {}
};

using namespace cv::v4d::event;
class MontageDemoPlan : public V4DPlan {
  using K = V4D::Keys;
  const cv::Size TILING_ = cv::Size(3, 3);

  std::vector<cv::Rect> targetViewports_;
  std::vector<cv::Ptr<Plan>> plans_;
  std::vector<std::string> labels_;

  cv::Rect defaultVP_;
  Event<Mouse> motion_ = E<Mouse>(Mouse::MOVE);
  Event<Mouse> releaseLeft_ = E<Mouse>(Mouse::RELEASE, Mouse::LEFT);
  Event<Mouse> releaseRight_ = E<Mouse>(Mouse::RELEASE, Mouse::RIGHT);

  struct State {
    int32_t focus_ = -1;
    int32_t zoomed_ = -1;
  };

  static State globalState_;
  State localState_;
  cv::Rect localVP_;
  string id_;

public:
  MontageDemoPlan() {
    plans_ = {_sub<CubeDemoPlan>(this),       _sub<ManyCubesDemoPlan>(this),
              _sub<VideoDemoPlan>(this),      _sub<NanoVGDemoPlan>(this),
              _sub<ShaderDemoPlan>(this, 15), _sub<BlankPlan>(this),
              _sub<BlankPlan>(this), _sub<BlankPlan>(this),
              _sub<BlankPlan>(this)};

    labels_ = {"Cube",        "Many Contexts", "Video Overlay",
               "Color Wheel", "Mandelbrot",    "Wall Of Text",
               "Pedestrian",  "Beauty",        "Optical Flow"};

    CV_Assert(size_t(TILING_.width * TILING_.height) == plans_.size());
  }

  void setup() override {
    cv::Size sz = V4D::get<cv::Size>(V4D::Keys::SIZE);
    defaultVP_ = V4D::get<cv::Rect>(V4D::Keys::VIEWPORT);

    int tw = sz.width / TILING_.width;
    int th = sz.height / TILING_.height;

    targetViewports_.resize(plans_.size());
    for (size_t x = 0; x < size_t(TILING_.width); ++x) {
      for (size_t y = 0; y < size_t(TILING_.height); ++y) {
        targetViewports_[x * TILING_.width + y] =
            cv::Rect(tw * x, th * y, tw, th);
      }
    }

    for (size_t i = 0; i < plans_.size(); ++i) {
      set(V4D::Keys::VIEWPORT, V(targetViewports_[i]));
      subSetup(plans_[i]);
    }
  }

  void infer() override {
    set(V4D::Keys::VIEWPORT, R(defaultVP_));
    set(V4D::Keys::CLEAR_COLOR, R(cv::Scalar(0, 0, 0, 255)));
    clear();

    plain(RW(localState_) = RS(globalState_));
    branch(R(localState_.zoomed_) == V(-1));
    {

      for (size_t i = 0; i < plans_.size(); ++i) {
        branch(R(localState_.focus_) == V(int32_t(i)));
        {
          set(V4D::Keys::VIEWPORT, R(targetViewports_[i]));
          capture();
          subInfer(plans_[i]);
          nvg(drawCell, R(defaultVP_));
        }
        endBranch();
      }

      for (size_t i = 0; i < plans_.size(); ++i) {
        branch(R(localState_.focus_) != V(int32_t(i)));
        {
          set(V4D::Keys::VIEWPORT, R(targetViewports_[i]));
          nvg(drawLabel, R(defaultVP_), R(labels_[i]));
        }
        endBranch();
      }
    }
    elseBranch();
    {
      set(V4D::Keys::VIEWPORT, V(defaultVP_));
      for (size_t i = 0; i < plans_.size(); ++i) {
        branch(R(localState_.zoomed_) == V(int32_t(i)));
        {
          capture();
          subInfer(plans_[i]);
        }
        endBranch();
      }
    }
    endBranch();

    set(V4D::Keys::DISABLE_INPUT_EVENTS, V(false));

    branch(BranchType::SINGLE, always_)
        ->plain(
            [](const Mouse::List &motion, const Mouse::List &reLeft,
               const Mouse::List &reRight,
               const std::vector<cv::Rect> &targetViewports, const cv::Rect& vp, State &state) {
              {
                using namespace cv::v4d::event;

                if (!motion.empty()) {
                  cv::Point loc = cv::Point(motion[0]->position().x, vp.height - motion[0]->position().y);
                  for (size_t i = 0; i < targetViewports.size(); ++i) {
                    if (targetViewports[i].contains(loc)) {
                      state.focus_ = i;
                      break;
                    }
                  }
                }

                if (state.zoomed_ > -1) {
                  if (!reRight.empty()) {
                    state.zoomed_ = -1;
                  }
                } else {
                  if (!reLeft.empty()) {
                    cv::Point loc = reLeft[0]->position();
                    for (size_t i = 0; i < targetViewports.size(); ++i) {
                      if (targetViewports[i].contains(loc)) {
                        state.zoomed_ = i;
                        break;
                      }
                    }
                  }
                }
              }
            },
            motion_, releaseLeft_, releaseRight_, R(targetViewports_), R(defaultVP_),
            RWS(globalState_))
        ->endBranch();

    set(V4D::Keys::DISABLE_INPUT_EVENTS, V(true));

    write();
  }

  void teardown() override {
    for (size_t i = 0; i < plans_.size(); ++i) {
      set(V4D::Keys::VIEWPORT, V(targetViewports_[i]));
      subTeardown(plans_[i]);
    }
  }
};

MontageDemoPlan::State MontageDemoPlan::globalState_;

int main(int argc, char **argv) {
  cv::samples::addSamplesDataSearchPath(V4D_ASSETS_PATH);

  std::string videoFile =
      (argc > 1) ? argv[1] : cv::samples::findFile("videos/bunny.mp4");
  if (videoFile.empty()) {
    cerr << "Usage: montage-demo <video-file>" << endl;
    return 1;
  }
  cv::Rect viewport(0, 0, 1280, 720);
  cv::Ptr<V4D> runtime = V4D::init(viewport, "Montage Demo",
                                   AllocateFlags::NANOVG | AllocateFlags::IMGUI,
                                   ConfigFlags::DISPLAY_MODE);
//  auto sink = Sink::make(runtime, "montage-demo.mkv", 60, viewport.size());
  auto src = Source::make(runtime, videoFile);
  runtime->setSource(src);
//  runtime->setSink(sink);
  V4DPlan::run<MontageDemoPlan>(0);

  return 0;
}
