// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include "samples.hpp"

// Runs every sample of this directory at the same time, each one in its own
// tile of a grid. The samples that are left out are the ones that bring their
// own runtime and cannot run as a sub-plan: two-windows-demo (one window per
// plan), pipeline-demo (composes samples itself), bgfx-demo/bgfx-demo2 (need
// the bgfx context), image_carousel-demo and imshow_reimplementation-demo
// (own the whole window).

// The include order matters. A sample that declares a class of its own in the
// global namespace has to be parsed before any sample that adds a
// `using namespace cv;`, or the two become ambiguous. optflow-demo declares
// ::SparseOpticalFlow, so it goes first. Everything else is free to follow in
// any order.

V4D_SAMPLE_ENTRY(v4d_optflow_main)(int argc, char **argv);
V4D_SAMPLE_ENTRY(v4d_cube_main)(int argc, char **argv);
V4D_SAMPLE_ENTRY(v4d_font_main)(int argc, char **argv);
V4D_SAMPLE_ENTRY(v4d_many_cubes_main)(int argc, char **argv);
V4D_SAMPLE_ENTRY(v4d_nanovg_main)(int argc, char **argv);
V4D_SAMPLE_ENTRY(v4d_pedestrian_main)(int argc, char **argv);
V4D_SAMPLE_ENTRY(v4d_shader_main)(int argc, char **argv);
V4D_SAMPLE_ENTRY(v4d_vector_graphics_main)(int argc, char **argv);
V4D_SAMPLE_ENTRY(v4d_video_main)(int argc, char **argv);
// Each sample declares its entry point as
// V4D_SAMPLE_ENTRY(V4D_SAMPLE_ENTRY_NAME), so redirecting the second one
// renames it in whichever TU this lands in -- main() on the host,
// v4ddemo_main() on Android, where two included samples defining the same name
// would otherwise be a redefinition.
#define V4D_SAMPLE_ENTRY_NAME v4d_optflow_main
#include "optflow-demo.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
#define V4D_SAMPLE_ENTRY_NAME v4d_cube_main
#include "cube-demo.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
#define V4D_SAMPLE_ENTRY_NAME v4d_font_main
#include "font-demo.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
#define V4D_SAMPLE_ENTRY_NAME v4d_many_cubes_main
#include "many_cubes-demo.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
#define V4D_SAMPLE_ENTRY_NAME v4d_nanovg_main
#include "nanovg-demo.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
#define V4D_SAMPLE_ENTRY_NAME v4d_pedestrian_main
#include "pedestrian-demo.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
#define V4D_SAMPLE_ENTRY_NAME v4d_shader_main
#include "shader-demo.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
#define V4D_SAMPLE_ENTRY_NAME v4d_vector_graphics_main
#include "vector_graphics.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
#define V4D_SAMPLE_ENTRY_NAME v4d_video_main
#include "video-demo.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
// Back to the platform name for this file's own entry point at the bottom.
#define V4D_SAMPLE_ENTRY_NAME V4D_SAMPLE_SELF_NAME

class MontageDemoPlan : public V4DPlan {
  using K = V4D::Keys;
  // Columns and rows of the grid, one tile per sub-plan. Must match the number
  // of sub-plans below.
  const cv::Size TILING_ = cv::Size(3, 3);
  std::vector<cv::Ptr<Plan>> plans_;

  cv::UMat currentFrame_;
  cv::UMat compositeFrame_;

public:
  MontageDemoPlan() {
    const std::string image = cv::samples::findFile("lena.png");
    plans_ = {
        _sub<OptflowDemoPlan>(this),    _sub<CubeDemoPlan>(this),
        _sub<FontDemoPlan>(this),       _sub<ManyCubesDemoPlan>(this),
        _sub<NanoVGDemoPlan>(this),     _sub<PedestrianDemoPlan>(this),
        _sub<ShaderDemoPlan>(this, 15), _sub<VectorGraphicsPlan>(this),
        _sub<VideoDemoPlan>(this),
    };

    CV_Assert(size_t(TILING_.width) * size_t(TILING_.height) == plans_.size());
  }

  void setup() override {
    for (size_t i = 0; i < plans_.size(); ++i) {
      subSetup(plans_[i]);
    }
  }

  void infer() override {
    fb(
        [](const cv::UMat &fb, cv::UMat &current, cv::UMat &composite) {
          fb.copyTo(current);
          fb.copyTo(composite);
        },
        RW(currentFrame_), RW(compositeFrame_));

    for (size_t i = 0; i < plans_.size(); ++i) {
      fb<1>([](const cv::UMat &frame, cv::UMat &fb) { frame.copyTo(fb); },
            R(currentFrame_));
      subInfer(plans_[i]);
      fb<1>(
          [](cv::UMat &composite, const cv::UMat &fb, const size_t &idx,
             const int &cols, const int &rows) {
            size_t w = fb.cols / cols;
            size_t h = fb.rows / rows;
            int x = w * (idx % cols);
            int y = h * (idx / cols);
            cv::resize(fb, composite(cv::Rect(x, y, w, h)), cv::Size(w, h));
          },
          RW(compositeFrame_), V(i), R(TILING_.width), R(TILING_.height));
    }
    fb<1>([](const cv::UMat &frame, cv::UMat &fb) { frame.copyTo(fb); },
          R(compositeFrame_));
  }

  void teardown() override {
    for (size_t i = 0; i < plans_.size(); ++i) {
      subTeardown(plans_[i]);
    }
  }
};

V4D_DEMO_MAIN(int argc, char **argv) {
  cv::v4d::add_asset_search_paths();

  std::string videoFile = demo_video_input("videos/kristen.mp4", argc, argv);
  if (videoFile.empty()) {
    cerr << "Usage: montage-demo <video-file>" << endl;
    return 1;
  }
  cv::Rect viewport(0, 0, 640, 480);
  cv::Ptr<V4D> runtime = V4D::init(
      viewport, "Montage Demo", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
  auto sink =
      Sink::makeDefault(runtime, "montage-demo.mkv", 30, viewport.size());
  auto src = Source::makeDefault(runtime, videoFile);
  runtime->setSource(src);
  runtime->setSink(sink);
  V4DPlan::run<MontageDemoPlan>(7);

  return 0;
}
