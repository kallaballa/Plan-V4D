// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Simple Counter Sample
// Demonstrates: Plan creation, plain() execution, edge DSL, branching,
//               property access, and the graph execution loop.

#include <opencv2/plan/plan.hpp>
#include <iostream>
#include <csignal>
#include <atomic>

using namespace cv;
using namespace cv::plan;

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running = false;
}

/*!
 * A simple plan that counts frames, computes statistics,
 * and branches based on counter value.
 */
class CounterPlan : public Plan {
public:
    // State
    uint64_t frameCount_ = 0;
    double fps_ = 0.0;
    uint64_t lastTimeNs_ = 0;
    std::string status_;

    // Configuration
    static constexpr uint64_t REPORT_INTERVAL = 60;  // report every N frames

    void setup() override {
        lastTimeNs_ = get_epoch_nanos();
        std::cout << "[CounterPlan] Setup complete." << std::endl;
    }

    void infer() override {
        // Increment frame counter
        plain([](uint64_t& cnt) { ++cnt; }, RW(frameCount_));

        // Compute FPS
        plain([](double& fps, uint64_t& lastTime) {
            uint64_t now = get_epoch_nanos();
            double dt = (now - lastTime) / 1e9;
            if (dt > 0.0) {
                fps = 1.0 / dt;
            }
            lastTime = now;
        }, RW(fps_), RW(lastTimeNs_));

        // Branch: print status periodically
        branch(R(frameCount_) % V(REPORT_INTERVAL) == V(uint64_t(0)))
            ->plain([](std::string& status, const uint64_t& cnt, const double& fps) {
                status = "Frame " + std::to_string(cnt) + " | FPS: " +
                         std::to_string(static_cast<int>(fps));
                std::cout << "  " << status << std::endl;
            }, RW(status_), R(frameCount_), R(fps_))
        ->endBranch();

        // Branch: milestone detection
        branch(R(frameCount_) == V(uint64_t(100)))
            ->plain([]() {
                std::cout << "  *** Milestone: 100 frames reached! ***" << std::endl;
            })
        ->endBranch();
    }

    void teardown() override {
        std::cout << "[CounterPlan] Teardown. Total frames: " << frameCount_ << std::endl;
    }
};

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int maxFrames = 300;
    if (argc > 1) {
        maxFrames = std::atoi(argv[1]);
    }

    std::cout << "=== Plan Module: Simple Counter Sample ===" << std::endl;
    std::cout << "Running for " << maxFrames << " frames (Ctrl+C to stop early)" << std::endl;
    std::cout << std::endl;

    // Initialize global state
    GlobalState::init_keys();
    LocalState::init_keys();

    // Create runtime and plan
    auto runtime = cv::makePtr<Runtime>(cv::Size(640, 480));
    auto plan = Plan::make<CounterPlan>();
    plan->setRuntime(runtime);

    // Setup phase
    plan->setup();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    // Main inference loop
    for (int i = 0; i < maxFrames && g_running; ++i) {
        GlobalState::apply<uint64_t>(GlobalState::Keys::FRAME_CNT,
            [](uint64_t& v) { ++v; return v; });

        plan->infer();
        plan->makeGraph();
        plan->runGraph();
        plan->clearGraph();

        // Simulate some work
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Teardown phase
    plan->teardown();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    std::cout << "\nDone." << std::endl;
    return 0;
}

