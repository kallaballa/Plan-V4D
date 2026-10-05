// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include "samples.hpp"

V4D_SAMPLE_ENTRY(v4d_video_main)(int argc, char **argv);
V4D_SAMPLE_ENTRY(v4d_nanovg_main)(int argc, char **argv);
V4D_SAMPLE_ENTRY(v4d_shader_main)(int argc, char **argv);
V4D_SAMPLE_ENTRY(v4d_pedestrian_main)(int argc, char **argv);
V4D_SAMPLE_ENTRY(v4d_optflow_main)(int argc, char **argv);
// See montage-demo.cpp: the samples below declare their entry point through
// V4D_SAMPLE_ENTRY_NAME, so redirecting that renames it on whichever host this
// is compiled for -- main() on the host, v4ddemo_main() on Android.
#define V4D_SAMPLE_ENTRY_NAME v4d_video_main
#include "video-demo.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
#define V4D_SAMPLE_ENTRY_NAME v4d_nanovg_main
#include "nanovg-demo.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
#define V4D_SAMPLE_ENTRY_NAME v4d_shader_main
#include "shader-demo.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
#define V4D_SAMPLE_ENTRY_NAME v4d_pedestrian_main
#include "pedestrian-demo.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
#define V4D_SAMPLE_ENTRY_NAME v4d_optflow_main
#include "optflow-demo.cpp"
#undef V4D_SAMPLE_ENTRY_NAME
// Back to the platform name for this file's own entry point at the bottom.
#define V4D_SAMPLE_ENTRY_NAME V4D_SAMPLE_SELF_NAME

using namespace cv::v4d::event;
class PipelineDemoPlan : public V4DPlan {
public:
  std::vector<cv::Ptr<Plan>> plans_;

  PipelineDemoPlan() {
    plans_ = {_sub<PedestrianDemoPlan>(this), _sub<VideoDemoPlan>(this),
              _sub<NanoVGDemoPlan>(this), _sub<ShaderDemoPlan>(this, 15)};
  }

  void setup() override {
    for (size_t i = 0; i < plans_.size(); ++i) {
      subSetup(plans_[i]);
    }
  }

  void infer() override {
    set(V4D::Keys::CLEAR_COLOR, R(cv::Scalar(0, 0, 0, 0)));
    for (size_t i = 0; i < plans_.size(); ++i) {
      subInfer(plans_[i]);
    }
  }

  void teardown() override {
    for (size_t i = 0; i < plans_.size(); ++i) {
      subTeardown(plans_[i]);
    }
  }
};

V4D_DEMO_MAIN(int argc, char **argv) {
  cv::v4d::add_asset_search_paths();

  std::string videoFile = demo_video_input("videos/dance.mp4", argc, argv);
  if (videoFile.empty()) {
    cerr << "Usage: pipeline-demo <video-file>" << endl;
    return 1;
  }
  cv::Rect viewport(0, 0, 1280, 720);
  cv::Ptr<V4D> runtime = V4D::init(
      viewport, "Pipeline Demo", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
  //  auto sink = Sink::make(runtime, "pipeline-demo.mkv", 60, viewport.size());
  auto src = Source::makeDefault(runtime, videoFile);
  runtime->setSource(src);
  //  runtime->setSink(sink);
  V4DPlan::run<PipelineDemoPlan>(3);

  return 0;
}
