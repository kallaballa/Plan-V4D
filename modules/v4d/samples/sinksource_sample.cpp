#include <opencv2/v4d/v4d.hpp>

using namespace cv;
using namespace cv::v4d;

class SinkSourceSamplePlan : public V4DPlan {
    const string msg_ = "SinkSource Sample";
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        capture();

        nvg([](const Size& sz, const string& str) {
            using namespace cv::v4d::nvg;

            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(Scalar(0, 255, 0, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0, sz.height / 2.0, str.c_str(), str.c_str() + str.size());
        }, sz_, R(msg_));

        write();
    }
};

int main(int argc, char **argv) {
    cv::samples::addSamplesDataSearchPath(V4D_ASSETS_PATH);

    std::string inputVideo =
            (argc > 1) ? argv[1] : cv::samples::findFile("videos/bunny.mp4");
    std::string outputVideo = (argc > 2) ? argv[2] : "sinksource_sample_out.mkv";
    if (inputVideo.empty()) {
        std::cerr
                << "Usage: sinksource_sample <input-video-file> <output-video-file>"
                << std::endl;
        return 1;
    }

    cv::Rect viewport(0, 0, 960, 960);
    Ptr<V4D> runtime = V4D::init(viewport, "SinkSource Sample", AllocateFlags::NANOVG | AllocateFlags::IMGUI, ConfigFlags::DISPLAY_MODE);

    auto ss = SinkSource::make(runtime, inputVideo, outputVideo);

    runtime->setSource(ss);
    runtime->setSink(ss);

    V4DPlan::run<SinkSourceSamplePlan>(2);
}
