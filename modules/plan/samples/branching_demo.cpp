// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Branching Demo
// Demonstrates: conditional execution, nested branches, BranchType variants,
//               edge predicates, and the else/end branch pattern.

#include <opencv2/plan/plan.hpp>
#include <iostream>
#include <random>
#include <cmath>

using namespace cv;
using namespace cv::plan;

/*!
 * A plan that classifies sensor readings and routes processing
 * through different branches based on value ranges.
 */
class SensorClassifierPlan : public Plan {
public:
    // Input
    double sensorValue_ = 0.0;

    // Output
    std::string classification_;
    double processedValue_ = 0.0;
    int lowCount_ = 0;
    int midCount_ = 0;
    int highCount_ = 0;
    int criticalCount_ = 0;

    void infer() override {
        // Multi-way classification using nested branches
        branch(R(sensorValue_) < V(25.0))
            // LOW range
            ->plain([](std::string& c, int& cnt) {
                c = "LOW";
                ++cnt;
            }, RW(classification_), RW(lowCount_))
            ->assign(RW(processedValue_), R(sensorValue_) * V(2.0))  // amplify
        ->elseBranch()
            ->branch(R(sensorValue_) < V(50.0))
                // MID range
                ->plain([](std::string& c, int& cnt) {
                    c = "MID";
                    ++cnt;
                }, RW(classification_), RW(midCount_))
                ->assign(RW(processedValue_), R(sensorValue_))  // pass through
            ->elseBranch()
                ->branch(R(sensorValue_) < V(75.0))
                    // HIGH range
                    ->plain([](std::string& c, int& cnt) {
                        c = "HIGH";
                        ++cnt;
                    }, RW(classification_), RW(highCount_))
                    ->assign(RW(processedValue_), R(sensorValue_) * V(0.5))  // attenuate
                ->elseBranch()
                    // CRITICAL range
                    ->plain([](std::string& c, int& cnt) {
                        c = "CRITICAL";
                        ++cnt;
                    }, RW(classification_), RW(criticalCount_))
                    ->assign(RW(processedValue_), V(0.0))  // clamp to zero
                ->endBranch()
            ->endBranch()
        ->endBranch();
    }
};

/*!
 * A plan demonstrating BranchType::ONCE - initialization that runs only once.
 */
class InitOncePlan : public Plan {
public:
    bool initialized_ = false;
    int runCount_ = 0;
    std::string initMessage_;

    void infer() override {
        // This branch executes only on the first call
        branch(BranchType::ONCE, always_)
            ->plain([](bool& init, std::string& msg) {
                init = true;
                msg = "System initialized at frame 1";
                std::cout << "  [ONCE] " << msg << std::endl;
            }, RW(initialized_), RW(initMessage_))
        ->endBranch();

        // This always runs
        plain([](int& cnt) { ++cnt; }, RW(runCount_));
    }
};

int main() {
    std::cout << "=== Plan Module: Branching Demo ===" << std::endl;
    std::cout << std::endl;

    GlobalState::init_keys();
    LocalState::init_keys();

    // --- Part 1: Sensor Classification ---
    std::cout << "--- Part 1: Sensor Classification ---" << std::endl;

    auto classifier = Plan::make<SensorClassifierPlan>();

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(0.0, 100.0);

    for (int i = 0; i < 20; ++i) {
        classifier->sensorValue_ = dist(rng);

        classifier->infer();
        classifier->makeGraph();
        classifier->runGraph();
        classifier->clearGraph();

        std::cout << "  Input: " << std::fixed << std::setprecision(1)
                  << classifier->sensorValue_
                  << " -> " << classifier->classification_
                  << " (processed: " << classifier->processedValue_ << ")"
                  << std::endl;
    }

    std::cout << "\n  Summary: LOW=" << classifier->lowCount_
              << " MID=" << classifier->midCount_
              << " HIGH=" << classifier->highCount_
              << " CRITICAL=" << classifier->criticalCount_
              << std::endl;

    // --- Part 2: BranchType::ONCE ---
    std::cout << "\n--- Part 2: BranchType::ONCE ---" << std::endl;

    auto initPlan = Plan::make<InitOncePlan>();

    for (int i = 0; i < 5; ++i) {
        initPlan->infer();
        initPlan->makeGraph();
        initPlan->runGraph();
        initPlan->clearGraph();

        std::cout << "  Frame " << (i + 1)
                  << ": initialized=" << initPlan->initialized_
                  << " runCount=" << initPlan->runCount_
                  << std::endl;
    }

    std::cout << "\nDone." << std::endl;
    return 0;
}

