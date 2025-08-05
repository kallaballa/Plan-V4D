#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class FontRenderingPlan: public Plan {
	//The text to render
	string text_ = "Hello World";
	Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
public:
	void infer() override {
		//Render the text at the center of the screen. Note that you can load you own fonts.
		nvg([](const Size& sz, const string& str) {
			using namespace cv::v4d::nvg;
			clearScreen();
			fontSize(40.0f);
			fontFace("sans-bold");
			fillColor(Scalar(255, 0, 0, 255));
			textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
			text(sz.width / 2.0, sz.height / 2.0, str.c_str(),
					str.c_str() + str.size());
		}, size_, R(text_));
	}
};

int main() {
	cv::Rect viewport(0, 0, 960, 960);
	cv::Ptr<V4D> runtime = V4D::init(viewport, "Font Rendering", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
	Plan::run<FontRenderingPlan>(0);

	return 0;
}
