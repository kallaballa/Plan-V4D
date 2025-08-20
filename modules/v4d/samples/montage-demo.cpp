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

static void drawCell(const cv::Rect& vp){
    using namespace cv::v4d::nvg;
    beginPath();
    strokeWidth(8);
    strokeColor(cv::Scalar(255,100,150,255));
    rect(8, 8, vp.width - 16, vp.height - 16);
    stroke();
}

static void drawLabel(const cv::Rect& vp, const string& label){
    using namespace cv::v4d::nvg;
    beginPath();
    strokeWidth(8);
    strokeColor(cv::Scalar(100,100,100,255));
    rect(8, 8, vp.width - 16, vp.height - 16);
    stroke();
    fontSize(120.0f);
    fontFace("sans-bold");
    fillColor(cv::Scalar(100, 100, 100, 255));
    textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    text(vp.size().width / 2.0, vp.size().height / 2.0 - 30.0, label.c_str(),
            label.c_str() + label.size());
}

static void drawSourceCodeLogo(const cv::Rect& vp, bool hover = false, const std::string& caption = "")
{
    using namespace cv::v4d::nvg;

    const float x = vp.x, y = vp.y, w = vp.width, h = vp.height;
    const float u  = std::min(w, h);          // scale unit
    const float r  = u * 0.18f;               // corner radius
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;

    // Colors (OpenCV uses BGR[A])
    const cv::Scalar shadow(0,   0,   0,   90);
    const cv::Scalar panel (36,  39,  45,  255);    // dark graphite
    const cv::Scalar panelHover(48, 52,  60,  255); // a touch brighter
    const cv::Scalar outlineHi(255, 255, 255, 30);
    const cv::Scalar outlineLo(0,   0,   0,   40);
    const cv::Scalar glyph (240, 240, 240, 255);    // light for </>
    const cv::Scalar accent(255, 170,  0,  255);    // cyan-ish for slash (BGR: 255,170,0)

    // --- Drop shadow
    beginPath();
    roundedRect(x + u*0.02f, y + u*0.03f, w - u*0.04f, h - u*0.04f, r);
    fillColor(shadow);
    fill();

    // --- Badge
    beginPath();
    roundedRect(x, y, w, h, r);
    fillColor(hover ? panelHover : panel);
    fill();

    // --- Soft outer + inner outlines for depth
    // Outer (dark)
    beginPath();
    roundedRect(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, r - 0.5f);
    strokeWidth(std::max(1.0f, u*0.015f));
    strokeColor(outlineLo);
    stroke();

    // Inner (light)
    beginPath();
    roundedRect(x + u*0.02f, y + u*0.02f, w - u*0.04f, h - u*0.04f, r - u*0.02f);
    strokeWidth(std::max(1.0f, u*0.01f));
    strokeColor(outlineHi);
    stroke();

    // --- </> glyph
    const float s  = u * 0.42f;                    // glyph size span
    const float th = std::max(2.0f, u * 0.085f);   // stroke thickness

    // Use round joins/caps if available
    #ifdef NVG_ROUND
    lineJoin(NVG_ROUND);
    lineCap(NVG_ROUND);
    #endif

    // Left chevron "<"
    beginPath();
    moveTo(cx - s*0.55f, cy);
    lineTo(cx - s*0.18f, cy - s*0.35f);
    moveTo(cx - s*0.55f, cy);
    lineTo(cx - s*0.18f, cy + s*0.35f);
    strokeWidth(th);
    strokeColor(glyph);
    stroke();

    // Right chevron ">"
    beginPath();
    moveTo(cx + s*0.18f, cy - s*0.35f);
    lineTo(cx + s*0.55f, cy);
    lineTo(cx + s*0.18f, cy + s*0.35f);
    strokeWidth(th);
    strokeColor(glyph);
    stroke();

    // Slash "/" accent
    beginPath();
    moveTo(cx - s*0.08f, cy - s*0.42f);
    lineTo(cx + s*0.08f, cy + s*0.42f);
    strokeWidth(th * 0.9f);
    strokeColor(accent);
    stroke();

    // --- "View" hint: a small eye up-right
    const float ex = x + w * 0.78f;
    const float ey = y + h * 0.28f;
    const float erx = u * 0.13f; // eye radius x
    const float ery = u * 0.085f; // eye radius y

    // Eye outline
    beginPath();
    ellipse(ex, ey, erx, ery);
    strokeWidth(std::max(1.0f, u*0.02f));
    strokeColor(cv::Scalar(220, 220, 220, 180));
    stroke();

    // Pupil
    beginPath();
    circle(ex, ey, std::min(erx, ery) * 0.32f);
    fillColor(cv::Scalar(50, 50, 50, 230));
    fill();

    // Tiny highlight
    beginPath();
    circle(ex + ery*0.15f, ey - ery*0.12f, std::max(1.0f, u*0.015f));
    fillColor(cv::Scalar(255, 255, 255, 200));
    fill();

    // --- Optional caption
    if (!caption.empty()) {
        fontFace("sans-bold");
        fontSize(u * 0.18f);
        fillColor(cv::Scalar(230, 230, 230, 220));
        textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
        text(cx, y + h - u*0.10f, caption.c_str(), caption.c_str() + caption.size());
    }
}

class BlankPlan : public Plan {
public:
	void infer() override {
	}
};

using namespace cv::v4d::event;
class MontageDemoPlan : public Plan {
    using K = V4D::Keys;
    const cv::Size TILING_ = cv::Size(3, 3);

    std::vector<cv::Rect> targetViewports_;
	std::vector<cv::Ptr<Plan>> plans_;
	std::vector<std::string> labels_;

	cv::Rect defaultVP_;
    cv::Rect sourceCodeVP_;
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
		plans_ = {
				_sub<CubeDemoPlan>(this),
				_sub<ManyCubesDemoPlan>(this),
				_sub<VideoDemoPlan>(this),
				_sub<NanoVGDemoPlan>(this),
				_sub<ShaderDemoPlan>(this, 15),
				_sub<FontDemoPlan>(this),
				_sub<PedestrianDemoPlan>(this),
				_sub<BeautyDemoPlan>(this),
				_sub<OptflowDemoPlan>(this)
			};

		labels_ = {
		        "Cube",
		        "Many Contexts",
		        "Video Overlay",
		        "Color Wheel",
		        "Mandelbrot",
		        "Wall Of Text",
		        "Pedestrian",
		        "Beauty",
		        "Optical Flow"
		};

		CV_Assert(size_t(TILING_.width * TILING_.height) == plans_.size());
	}

	void setup() override {
        cv::Size sz = V4D::get<cv::Size>(V4D::Keys::SIZE);
        defaultVP_ = V4D::get<cv::Rect>(V4D::Keys::VIEWPORT);
        size_t wSC = defaultVP_.width / 8;
        size_t hSC = defaultVP_.height / 8;
        sourceCodeVP_ = cv::Rect(wSC / 4, sz.height - (hSC + hSC / 4), wSC, hSC);

        int tw = sz.width / TILING_.width;
        int th = sz.height / TILING_.height;

        targetViewports_.resize(plans_.size());
        for(size_t x = 0; x < size_t(TILING_.width); ++x) {
            for(size_t y = 0; y < size_t(TILING_.width); ++y) {
                targetViewports_[x * 3 + y] = cv::Rect(tw * x, th * y, tw, th);
            }
        }

        for(size_t i = 0; i < plans_.size(); ++i) {
            set(V4D::Keys::VIEWPORT, V(targetViewports_[i]));
            subSetup(plans_[i]);
        }
	}

	void infer() override {
        set(V4D::Keys::VIEWPORT, R(defaultVP_));
        set(V4D::Keys::CLEAR_COLOR, R(cv::Scalar(0,0,0,255)));
        clear();

        plain(RW(localState_) = RS(globalState_));
		branch(R(localState_.zoomed_) == V(-1));
		{

		    for(size_t i = 0; i < plans_.size(); ++i) {
                branch(R(localState_.focus_) == V(int32_t(i)));
                {
                    set(V4D::Keys::VIEWPORT, R(targetViewports_[i]));
                    capture();
                    subInfer(plans_[i]);
                    nvg(drawCell, R(defaultVP_));
                }
                endBranch();
            }

            for(size_t i = 0; i < plans_.size(); ++i) {
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
		    for(size_t i = 0; i < plans_.size(); ++i) {
				branch(CS(globalState_.zoomed_) == V(int32_t(i)));
				{
			        capture();
				    subInfer(plans_[i]);
				    nvg(drawSourceCodeLogo, R(sourceCodeVP_), V(false), V("Source"));
				}
				endBranch();
			}
		}
		endBranch();

        set(V4D::Keys::DISABLE_INPUT_EVENTS, V(false));

		branch(BranchType::SINGLE, always_)
            ->plain([](const Mouse::List& motion, const Mouse::List& reLeft, const Mouse::List& reRight, const std::vector<cv::Rect>& targetViewports, State& state) {
				{
					using namespace cv::v4d::event;

                    if(!motion.empty()) {
                        cv::Point loc = motion[0]->position();
                        for(size_t i = 0; i < targetViewports.size(); ++i) {
                            if(targetViewports[i].contains(loc)) {
                                state.focus_ = i;
                                break;
                            }
                        }
                    }

					if(globalState_.zoomed_ > -1) {
						if(!reRight.empty()) {
							globalState_.zoomed_ = -1;
						}
					} else {
						if(!reLeft.empty()) {
							cv::Point loc = reLeft[0]->position();
							for(size_t i = 0; i < targetViewports.size(); ++i) {
								if(targetViewports[i].contains(loc)) {
									state.zoomed_ = i;
									break;
								}
							}
						}
					}
				}
			}, motion_, releaseLeft_, releaseRight_, R(targetViewports_), RWS(globalState_))
		->endBranch();

        set(V4D::Keys::DISABLE_INPUT_EVENTS, V(true));

        write();
	}

	void teardown() override {
        for(size_t i = 0; i < plans_.size(); ++i) {
            set(V4D::Keys::VIEWPORT, V(targetViewports_[i]));
            subTeardown(plans_[i]);
        }
	}
};

MontageDemoPlan::State MontageDemoPlan::globalState_;

int main(int argc, char** argv) {
	if (argc != 3) {
        cerr << "Usage: montage-demo <video-file> <number of extra workers>" << endl;
        exit(1);
    }
	cv::Rect viewport(0, 0, 1920, 1080);
    cv::Ptr<V4D> runtime = V4D::init(viewport, "Montage Demo", AllocateFlags::NANOVG | AllocateFlags::IMGUI, ConfigFlags::DISPLAY_MODE);
    auto sink = Sink::make(runtime, "montage-demo.mkv", 60, viewport.size());
    auto src = Source::make(runtime, argv[1]);
    runtime->setSource(src);
    runtime->setSink(sink);
    Plan::run<MontageDemoPlan>(atoi(argv[2]));

    return 0;
}

