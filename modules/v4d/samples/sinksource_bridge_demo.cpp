// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

// Two plans, two windows, one SinkSource bridge. The producer plan captures
// frames and pushes them into the shared SinkSource; the consumer plan pulls
// them back out and renders them. Closing one window stops that plan only.

#include <opencv2/v4d/v4d.hpp>

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

using namespace cv;
using namespace cv::v4d;

class ProducerPlan : public V4DPlan {
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        capture();

        nvg([](const Size& sz) {
            using namespace cv::v4d::nvg;
            clearScreen(cv::Scalar(24, 24, 40, 255));

            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(cv::Scalar(80, 200, 255, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0f, sz.height / 2.0f - 20.0f, "Producer", nullptr);
        }, sz_);

        write();
    }
};

class ConsumerPlan : public V4DPlan {
    Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
    void infer() override {
        capture();

        nvg([](const Size& sz) {
            using namespace cv::v4d::nvg;
            clearScreen(cv::Scalar(24, 24, 40, 255));

            fontSize(40.0f);
            fontFace("sans-bold");
            fillColor(cv::Scalar(255, 160, 80, 255));
            textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            text(sz.width / 2.0f, sz.height / 2.0f - 20.0f, "Consumer", nullptr);
        }, sz_);
    }
};

int main(int argc, char** argv) {
    cv::samples::addSamplesDataSearchPath(V4D_ASSETS_PATH);

    std::string inputVideo =
            (argc > 1) ? argv[1] : cv::samples::findFile("videos/bunny.mp4");
    if(inputVideo.empty()) {
        std::cerr << "Usage: sinksource_bridge_demo <input-video-file>" << std::endl;
        return 1;
    }

    cv::Ptr<SinkSource> ss = SinkSource::make(30.0f);

    std::thread producer([inputVideo, ss]() {
        V4D::init(cv::Rect(0, 0, 480, 360), "Producer",
            AllocateFlags::NANOVG | AllocateFlags::IMGUI, ConfigFlags::DISPLAY_MODE);

        auto src = Source::make(V4D::instance(), inputVideo);
        V4D::instance()->setSource(src);

        cv::Ptr<Sink> sink = new Sink([ss](const uint64_t& seq, const cv::UMat& frame) {
            CV_UNUSED(seq);
            ss->operator()(seq, frame);
            return ss->isOpen();
        });
        V4D::instance()->setSink(sink);

        V4DPlan::run<ProducerPlan>(0);
    });

    V4D::init(cv::Rect(480, 0, 480, 360), "Consumer",
        AllocateFlags::NANOVG | AllocateFlags::IMGUI, ConfigFlags::DISPLAY_MODE);

    cv::Ptr<Source> src = new Source([ss](cv::UMat& frame) {
        frame = ss->operator()();
        return !frame.empty();
    }, 30.0f);
    V4D::instance()->setSource(src);

    V4DPlan::run<ConsumerPlan>(0);

    producer.join();
    ss->close();
    std::cout << "Both plans finished." << std::endl;
    return 0;
}
