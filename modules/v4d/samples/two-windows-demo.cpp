// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

// Two plans, two windows, one process. One plan runs on the main thread, the
// other one on a dedicated thread. Both keep their own frame counters, their own
// frame synchronization, their own GUI and their own input queue, and closing one
// window ends that plan only - the other one keeps running.

#include <opencv2/v4d/v4d.hpp>

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

using namespace cv;
using namespace cv::v4d;

// A spinning triangle, so the two windows are easy to tell apart.
class TrianglePlan : public V4DPlan {
	std::string label_;
	cv::Scalar color_;
	size_t frames_ = 0;
	// Number of frames after which this plan stops its own run, 0 = run until
	// the window is closed.
	size_t maxFrames_ = 0;
	Property<cv::Size> sz_ = P<cv::Size>(V4D::Keys::SIZE);
public:
	TrianglePlan(const std::string& label, const cv::Scalar& color, size_t maxFrames = 0)
	    : label_(label), color_(color), maxFrames_(maxFrames) {
	}

	void infer() override {
		nvg([](const cv::Size& sz) {
			using namespace cv::v4d::nvg;
			clearScreen(cv::Scalar(24, 24, 40, 255));

			static long start = cv::getTickCount() / cv::getTickFrequency();
			float angle = (cv::getTickCount() / cv::getTickFrequency() - start) * 90.0f;
			float cx = sz.width / 2.0f;
			float cy = sz.height / 2.0f;
			float r = std::min(sz.width, sz.height) / 4.0f;

			save();
			translate(cx, cy);
			rotate(angle);
			beginPath();
			moveTo(0, -r);
			lineTo(r * 0.866f, r * 0.5f);
			lineTo(-r * 0.866f, r * 0.5f);
			closePath();
			fillColor(cv::Scalar(80, 200, 255, 255));
			fill();
			restore();
		}, sz_);

		plain([this]() {
			// A node of a plan may stop its own run, which is per runtime, i.e.
			// it does not affect the other plan of this process.
			if(maxFrames_ > 0 && ++frames_ >= maxFrames_)
				V4D::instance()->requestFinish();
		});
	}

	void gui() override {
		// gui() is a one-shot setup hook that runs on the display thread before
		// the first frame, so there is no ImGui frame in scope yet. ImGui drawing
		// therefore goes into an imgui() node, which is executed inside the ImGui
		// frame of this plan's own window, on this plan's own display thread, with
		// this window's ImGui context made current.
		imgui([label = label_]() {			using namespace ImGui;
			SetNextWindowPos(ImVec2(10, 10));
			Begin(label.c_str());
			Text("Displayed by thread %d",
			    (int)std::hash<std::thread::id>{}(std::this_thread::get_id()));
			End();
		});
	}

	void teardown() override {
		std::cout << label_ << " ran " << frames_ << " frames on thread "
		          << std::hash<std::thread::id>{}(std::this_thread::get_id()) << std::endl;
	}
};

// Runs a plan with its own window on its own thread and returns once that window
// was closed. This is all it takes to add a window to a program: the runtime, the
// plan state and the input queue are per thread, and a plan only synchronizes
// with its own workers.
template<typename Tplan>
static void runPlanInThread(const std::string& title, const cv::Rect& viewport, const cv::Scalar& color, size_t maxFrames) {
    std::thread t([title, viewport, color, maxFrames]() {
        V4D::init(viewport, title, AllocateFlags::NANOVG | AllocateFlags::IMGUI, ConfigFlags::DISPLAY_MODE);
        V4DPlan::run<Tplan>(0, title, color, maxFrames);
    });
    t.join();
}

int main(int argc, char** argv) {
    // Both plans stop themselves after 5 seconds unless --no-auto-close is given,
    // so that the demo terminates on its own. Close one of the two windows by
    // hand to see that only that plan stops.
    const bool autoClose = (argc < 2 || std::string(argv[1]) != "--no-auto-close");
    const size_t maxFrames = autoClose ? size_t(5 * 60) : 0;
    constexpr size_t extraWorkers = 1; // one worker per plan, plus its display thread

    std::thread second([maxFrames]() {
        runPlanInThread<TrianglePlan>("Threaded triangle", cv::Rect(0, 0, 480, 360),
            cv::Scalar(80, 200, 255, 255), maxFrames);
    });

    // The main thread displays the second window while the first one is running.
    V4D::init(cv::Rect(0, 0, 480, 360), "Main triangle",
        AllocateFlags::NANOVG | AllocateFlags::IMGUI, ConfigFlags::DISPLAY_MODE);
    V4DPlan::run<TrianglePlan>(extraWorkers, "Main triangle", cv::Scalar(255, 160, 80, 255), maxFrames);

    second.join();
    std::cout << "Both plans finished." << std::endl;
    return 0;
}
