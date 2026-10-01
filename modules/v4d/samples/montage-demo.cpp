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

class MontageDemoPlan : public V4DPlan {
  using K = V4D::Keys;
  const cv::Size TILING_ = cv::Size(2, 2);
  std::vector<cv::Ptr<Plan>> plans_;

  cv::UMat currentFrame_;
  cv::UMat compositeFrame_;
public:
  MontageDemoPlan() {
    plans_ = { 
	       _sub<NanoVGDemoPlan>(this),
               _sub<ShaderDemoPlan>(this, 15), 
	       _sub<BeautyDemoPlan>(this),
               _sub<OptflowDemoPlan>(this),
              };

    CV_Assert(size_t(TILING_.width * TILING_.height) == plans_.size());
  }

  void setup() override {
    for (size_t i = 0; i < plans_.size(); ++i) {
      subSetup(plans_[i]);
    }
  }

  void infer() override {
    fb([](const cv::UMat& fb, cv::UMat& current, cv::UMat& composite) { 
      fb.copyTo(current);
      fb.copyTo(composite); 
    }, RW(currentFrame_), RW(compositeFrame_));

    for (size_t i = 0; i < plans_.size(); ++i) {
      fb<1>([](const cv::UMat& frame, cv::UMat& fb){ frame.copyTo(fb); }, R(currentFrame_));
      subInfer(plans_[i]);
      fb<1>([](cv::UMat& composite, const cv::UMat& fb, const size_t& idx, const int& tiling) {
        size_t w = fb.cols / tiling;
        size_t h = fb.rows / tiling;
        int x = w * ((idx / tiling) % tiling);
	int y = h * (idx % tiling);
	cv::resize(fb, composite(cv::Rect(x, y, w, h)), cv::Size(w, h));
      }, RW(compositeFrame_), V(i), R(TILING_.width));
    }
    fb<1>([](const cv::UMat& frame, cv::UMat& fb){ frame.copyTo(fb); }, R(compositeFrame_));
  }

  void teardown() override {
    for (size_t i = 0; i < plans_.size(); ++i) {
      subTeardown(plans_[i]);
    }
  }
};


int main(int argc, char **argv) {
  cv::v4d::add_asset_search_paths();

  std::string videoFile =
      (argc > 1) ? argv[1] : cv::samples::findFile("videos/kristen.mp4");
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
  V4DPlan::run<MontageDemoPlan>(3);

  return 0;
}
