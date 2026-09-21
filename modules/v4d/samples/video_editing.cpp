#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class VideoEditingPlan : public V4DPlan {
	cv::UMat frame_;
	const string hv_ = "Hello Video!";
	//Property extends Edge which means it can be directly passed without Edge-directive
	Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
	void infer() override {
		//Capture video from the source
		capture();

		//Render on top of the video
		nvg([](const Size& sz, const string& str) {
			using namespace cv::v4d::nvg;

			fontSize(40.0f);
			fontFace("sans-bold");
			fillColor(Scalar(255, 0, 0, 255));
			textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
			text(sz.width / 2.0, sz.height / 2.0, str.c_str(), str.c_str() + str.size());
		}, sz_, R(hv_));

		//Write video to the sink
		write();
	}
};

int main(int argc, char **argv) {
	cv::samples::addSamplesDataSearchPath (V4D_ASSETS_PATH);

	std::string inputVideo =
			(argc > 1) ? argv[1] : cv::samples::findFile("videos/bunny.mp4");
	std::string outputVideo = (argc > 2) ? argv[2] : "video_editing_out.mkv";
	if (inputVideo.empty()) {
		std::cerr
				<< "Usage: video_editing <input-video-file> <output-video-file>"
				<< std::endl;
		return 1;
	}

	cv::Rect viewport(0, 0, 960, 960);
	Ptr<V4D> runtime = V4D::init(viewport, "Video Editing", AllocateFlags::NANOVG | AllocateFlags::IMGUI, ConfigFlags::DISPLAY_MODE);

	//Make the video source
	auto src = Source::make(runtime, inputVideo);

	//Make the video sink
	auto sink = Sink::make(runtime, outputVideo, src->fps(), viewport.size());

	//Attach source and sink
	runtime->setSource(src);
	runtime->setSink(sink);

	V4DPlan::run<VideoEditingPlan>(2);
}
