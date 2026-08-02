// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Sub-Plan Composition Demo
// Demonstrates: sub-plan creation, subInfer/subSetup/subTeardown,
//               hierarchical plan composition, and data flow between plans.

#include <opencv2/plan/plan.hpp>
#include <iostream>
#include <cmath>
#include <deque>

using namespace cv;
using namespace cv::plan;

/*!
 * Sub-plan: Computes a running average over a window.
 */
class WindowedAveragePlan : public Plan {
public:
    // Interface
    double input_ = 0.0;
    double output_ = 0.0;

    // Internal state
    std::deque<double> window_;
    size_t windowSize_ = 5;

    WindowedAveragePlan(size_t windowSize = 5) : windowSize_(windowSize) {}

    void infer() override {
        plain([](std::deque<double>& win, double& out, const double& in, const size_t& sz) {
            win.push_back(in);
            while (win.size() > sz) {
                win.pop_front();
            }
            double sum = 0.0;
            for (double v : win) sum += v;
            out = sum / win.size();
        }, RW(window_), RW(output_), R(input_), R(windowSize_));
    }
};

/*!
 * Sub-plan: Detects threshold crossings.
 */
class ThresholdDetectorPlan : public Plan {
public:
    // Interface
    double input_ = 0.0;
    double threshold_ = 50.0;
    bool crossing_ = false;
    bool above_ = false;

    // Internal
    bool prevState_ = false;

    void infer() override {
        plain([](bool& crossing, bool& above, bool& prev, const double& in, const double& thresh) {
            bool current = in > thresh;
            crossing = (current != prev);
            above = current;
            prev = current;
        }, RW(crossing_), RW(above_), RW(prevState_), R(input_), R(threshold_));
    }
};

/*!
 * Sub-plan: Generates a simple signal (sine wave + noise).
 */
class SignalGeneratorPlan : public Plan {
public:
    // Interface
    double output_ = 0.0;
    double frequency_ = 0.1;
    double amplitude_ = 40.0;
    double offset_ = 50.0;

    // Internal
    uint64_t sampleCount_ = 0;

    void infer() override {
        plain([](double& out, uint64_t& cnt, const double& freq, const double& amp, const double& off) {
            out = off + amp * std::sin(2.0 * CV_PI * freq * cnt);
            ++cnt;
        }, RW(output_), RW(sampleCount_), R(frequency_), R(amplitude_), R(offset_));
    }
};

/*!
 * Parent plan: Composes signal generation, smoothing, and threshold detection.
 */
class SignalProcessingPipeline : public Plan {
public:
    // Sub-plans
    cv::Ptr<SignalGeneratorPlan> generator_;
    cv::Ptr<WindowedAveragePlan> smoother_;
    cv::Ptr<ThresholdDetectorPlan> detector_;

    // Pipeline output
    double rawSignal_ = 0.0;
    double smoothSignal_ = 0.0;
    bool thresholdCrossing_ = false;
    int crossingCount_ = 0;

    SignalProcessingPipeline() {
        generator_ = _sub<SignalGeneratorPlan>(this);
        smoother_ = _sub<WindowedAveragePlan>(this, 5);
        detector_ = _sub<ThresholdDetectorPlan>(this);
    }

    void setup() override {
        subSetup(generator_);
        subSetup(smoother_);
        subSetup(detector_);
        std::cout << "  [Pipeline] All sub-plans set up." << std::endl;
    }

    void infer() override {
        // Stage 1: Generate signal
        subInfer(generator_);
        assign(RW(rawSignal_), R(generator_->output_));

        // Stage 2: Smooth the signal
        assign(RW(smoother_->input_), R(rawSignal_));
        subInfer(smoother_);
        assign(RW(smoothSignal_), R(smoother_->output_));

        // Stage 3: Detect threshold crossings
        assign(RW(detector_->input_), R(smoothSignal_));
        subInfer(detector_);
        assign(RW(thresholdCrossing_), R(detector_->crossing_));

        // Count crossings
        branch(R(thresholdCrossing_))
            ->plain([](int& cnt) { ++cnt; }, RW(crossingCount_))
        ->endBranch();
    }

    void teardown() override {
        subTeardown(generator_);
        subTeardown(smoother_);
        subTeardown(detector_);
        std::cout << "  [Pipeline] All sub-plans torn down." << std::endl;
    }
};

int main() {
    std::cout << "=== Plan Module: Sub-Plan Composition Demo ===" << std::endl;
    std::cout << std::endl;

    GlobalState::init_keys();
    LocalState::init_keys();

    auto pipeline = Plan::make<SignalProcessingPipeline>();

    // Setup
    pipeline->setup();
    pipeline->makeGraph();
    pipeline->runGraph();
    pipeline->clearGraph();

    // Run the pipeline
    std::cout << "  Running signal processing pipeline (50 samples):" << std::endl;
    std::cout << "  " << std::string(60, '-') << std::endl;

    for (int i = 0; i < 50; ++i) {
        pipeline->infer();
        pipeline->makeGraph();
        pipeline->runGraph();
        pipeline->clearGraph();

        if (i % 5 == 0 || pipeline->thresholdCrossing_) {
            std::cout << "  [" << std::setw(2) << i << "]"
                      << " raw=" << std::fixed << std::setprecision(1) << std::setw(6) << pipeline->rawSignal_
                      << " smooth=" << std::setw(6) << pipeline->smoothSignal_
                      << (pipeline->thresholdCrossing_ ? " ** CROSSING **" : "")
                      << std::endl;
        }
    }

    std::cout << "  " << std::string(60, '-') << std::endl;
    std::cout << "  Total threshold crossings: " << pipeline->crossingCount_ << std::endl;

    // Teardown
    pipeline->teardown();
    pipeline->makeGraph();
    pipeline->runGraph();
    pipeline->clearGraph();

    std::cout << "\nDone." << std::endl;
    return 0;
}

