// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
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

class BlankPlan : public Plan {
public:
	void infer() override {
		clear();
	}
};

using namespace cv::v4d::event;
class MontageDemoPlan : public Plan {
    using K = V4D::Keys;
    const cv::Size TILING_ = cv::Size(3, 3);

	std::vector<cv::Rect> targetViewports_;
	std::vector<cv::Ptr<Plan>> plans_;

	Property<cv::Size> size_ = P<cv::Size>(K::SIZE);
    Property<cv::Size> windowSize_ = P<cv::Size>(K::WINDOW_SIZE);

	Event<Mouse> releaseLeft = E<Mouse>(Mouse::RELEASE, Mouse::LEFT);
	Event<Mouse> releaseRight = E<Mouse>(Mouse::RELEASE, Mouse::RIGHT);

	struct State {
		int32_t lastZoomed_ = -1;
		int32_t zoomed_ = -1;
	};

	static State state_;
	string id_;
	cv::Rect defaultVP_;
public:
	MontageDemoPlan() {
//		plans_ = {
//				_sub<CubeDemoPlan>(this),
//				_sub<ManyCubesDemoPlan>(this),
//				_sub<VideoDemoPlan>(this),
//				_sub<NanoVGDemoPlan>(this),
//				_sub<ShaderDemoPlan>(this, 15),
//				_sub<FontDemoPlan>(this),
//				_sub<BlankPlan>(this),
////				_sub<PedestrianDemoPlan>(this),
//				_sub<BeautyDemoPlan>(this),
//				_sub<BlankPlan>(this),
////				_sub<OptflowDemoPlan>(this)
//			};

        plans_ = {
                _sub<CubeDemoPlan>(this),
                _sub<BlankPlan>(this),
                _sub<BlankPlan>(this),
                _sub<BlankPlan>(this),
                _sub<BlankPlan>(this),
                _sub<BlankPlan>(this),
                _sub<BlankPlan>(this),
                _sub<BlankPlan>(this),
                _sub<BlankPlan>(this),
            };

	    CV_Assert(size_t(TILING_.width * TILING_.height) == plans_.size());
	}

	void setup() override {
        cv::Size sz = V4D::get<cv::Size>(V4D::Keys::SIZE);
        defaultVP_ = cv::Rect(0, 0, sz.width, sz.height);
        int w = sz.width / TILING_.width;
        int h = sz.height / TILING_.height;

        targetViewports_.resize(plans_.size());
        for(size_t x = 0; x < size_t(TILING_.width); ++x) {
            for(size_t y = 0; y < size_t(TILING_.width); ++y) {
                targetViewports_[x * 3 + y] = cv::Rect(w * x, h * y, w, h);
            }
        }

	    for(size_t i = 0; i < plans_.size(); ++i) {
	        set(K::VIEWPORT, V(targetViewports_[i]));
		    subSetup(plans_[i]);
		}
	}

	void infer() override {
//		set(K::VIEWPORT, V(defaultVP_));
		set(K::DISABLE_INPUT_EVENTS, V(true));
//		clear();


		branch(CS(state_.zoomed_) == V(-1));
		{
		    for(size_t i = 0; i < plans_.size(); ++i) {
	            set(K::VIEWPORT, V(targetViewports_[i]));
	            subInfer(plans_[i]);
		    }
		}
		elseBranch();
		{
            set(K::VIEWPORT, V(defaultVP_));

		    for(size_t i = 0; i < plans_.size(); ++i) {
				branch(CS(state_.zoomed_) == V(int32_t(i)));
				{
	                subInfer(plans_[i]);
				}
				endBranch();
			}
		}
		endBranch();

		set(K::DISABLE_INPUT_EVENTS, V(false));

		//pinned to the first worker
		branch(0, always_)
			->plain([](const cv::Size& sz, const cv::Size& winSz, const Mouse::List& reLeft, const Mouse::List& reRight, const std::vector<cv::Rect>& targetViewports, State& state) {
				{
					using namespace cv::v4d::event;
					const double scaleX = double(sz.width) / winSz.width;
					const double scaleY = double(sz.height) / winSz.height;
					const double scale = std::min(scaleX, scaleY);
					if(state_.zoomed_ > -1) {
						if(!reRight.empty()) {
							state_.zoomed_ = -1;
						}
					} else {
						if(!reLeft.empty()) {
							cv::Point loc = reLeft[0]->position() * scale;
							for(size_t i = 0; i < targetViewports.size(); ++i) {
								if(targetViewports[i].contains(loc)) {
									state.zoomed_ = i;
									break;
								}
							}
						}
					}
				}
			}, size_, windowSize_, releaseLeft, releaseRight, R(targetViewports_), RWS(state_))
		->endBranch();
	}


	void teardown() override {
        for(size_t i = 0; i < plans_.size(); ++i) {
            set(K::VIEWPORT, V(targetViewports_[i]));
            subTeardown(plans_[i]);
        }
	}
};

MontageDemoPlan::State MontageDemoPlan::state_;

int main(int argc, char** argv) {
	if (argc != 3) {
        cerr << "Usage: montage-demo <video-file> <number of extra workers>" << endl;
        exit(1);
    }
	cv::Rect viewport(0, 0, 1280, 720);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Montage Demo", AllocateFlags::NANOVG | AllocateFlags::IMGUI, ConfigFlags::DEFAULT);
    auto src = Source::make(runtime, argv[1]);
    runtime->setSource(src);
    Plan::run<MontageDemoPlan>(atoi(argv[2]));

    return 0;
}

