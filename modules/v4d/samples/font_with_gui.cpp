#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class FontWithGuiPlan: public Plan {
	static struct Params {
		float frontSize_ = 40.0f;
		cv::Scalar_<float> color_ = {1.0f, 0.0f, 0.0f, 1.0f};
	} params_;
	//The text
	string hw_ = "hello world";
	Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
	void gui() override {
		imgui([](Params& params) {
			using namespace ImGui;
			Begin("Settings");
			SliderFloat("Font Size", &params.frontSize_, 1.0f, 100.0f);
			ColorPicker4("Text Color", params.color_.val);
			End();
		}, params_);
	}

	void infer() override {
		//Render the text at the center of the screen using parameters from the GUI.
		nvg([](const cv::Size& sz, const string& str, const Params& params) {
			using namespace cv::v4d::nvg;
			clearScreen();
			fontSize(params.frontSize_);
			fontFace("sans-bold");
			fillColor(params.color_ * 255.0);
			textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
			text(sz.width / 2.0, sz.height / 2.0, str.c_str(), str.c_str() + str.size());
		}, size_, R(hw_), CS(params_));
	}
};

FontWithGuiPlan::Params FontWithGuiPlan::params_;

int main() {
	cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "Font Rendering with GUI", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
	Plan::run<FontWithGuiPlan>(0);
}

