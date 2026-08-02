// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.

#include "opencv2/plan/plan.hpp"
#include "opencv2/core.hpp"
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>

using namespace cv;
using namespace cv::plan;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            ++tests_failed; \
        } else { \
            ++tests_passed; \
        } \
    } while(0)

// Sub-plan that computes a moving average
class MovingAveragePlan : public Plan {
public:
    double sum_ = 0.0;
    int count_ = 0;
    double average_ = 0.0;

    void infer() override {
        plain([](double& sum, int& count, double& avg, const double& input) {
            sum += input;
            ++count;
            avg = sum / count;
        }, RW(sum_), RW(count_), RW(average_), R(input_));
    }

    double input_ = 0.0;
};

// Parent plan that uses the sub-plan
class ParentPlan : public Plan {
public:
    double sensorValue_ = 0.0;
    double smoothedValue_ = 0.0;
    cv::Ptr<MovingAveragePlan> avgPlan_;

    ParentPlan() {
        avgPlan_ = _sub<MovingAveragePlan>(this);
    }

    void infer() override {
        // Feed sensor value to sub-plan
        assign(RW(avgPlan_->input_), R(sensorValue_));

        // Run sub-plan inference
        subInfer(avgPlan_);

        // Read result back
        assign(RW(smoothedValue_), R(avgPlan_->average_));
    }
};

// Pipeline plan with setup/infer/teardown phases
class LifecyclePlan : public Plan {
public:
    bool setupDone_ = false;
    bool inferDone_ = false;
    bool teardownDone_ = false;
    int frameCount_ = 0;

    void setup() override {
        setupDone_ = true;
        frameCount_ = 0;
    }

    void infer() override {
        ++frameCount_;
        inferDone_ = true;
    }

    void teardown() override {
        teardownDone_ = true;
    }
};

// Plan that uses multiple contexts
class MultiContextPlan : public Plan {
public:
    int plainResult_ = 0;
    int customResult_ = 0;

    void infer() override {
        plain([](int& r) { r = 1; }, RW(plainResult_));
        ctx("custom", [](int& r) { r = 2; }, RW(customResult_));
    }
};

// Plan demonstrating the full edge DSL in a pipeline
class EdgeDSLPlan : public Plan {
public:
    int a_ = 10;
    int b_ = 3;
    int sum_ = 0;
    int diff_ = 0;
    int prod_ = 0;
    int quot_ = 0;
    int mod_ = 0;
    bool isGreater_ = false;
    int maxVal_ = 0;

    void infer() override {
        assign(RW(sum_), R(a_) + R(b_));
        assign(RW(diff_), R(a_) - R(b_));
        assign(RW(prod_), R(a_) * R(b_));
        assign(RW(quot_), R(a_) / R(b_));
        assign(RW(mod_), R(a_) % R(b_));
        assign(RW(isGreater_), R(a_) > R(b_));
        assign(RW(maxVal_), IF(R(a_) > R(b_), R(a_), R(b_)));
    }
};

void test_subplan_composition() {
    auto plan = Plan::make<ParentPlan>();

    // Feed values
    double inputs[] = {10.0, 20.0, 30.0, 40.0, 50.0};
    for (double val : inputs) {
        plan->sensorValue_ = val;
        plan->infer();
        plan->makeGraph();
        plan->runGraph();
        plan->clearGraph();
    }

    // Average of 10,20,30,40,50 = 30
    TEST_ASSERT(std::abs(plan->smoothedValue_ - 30.0) < 0.001,
                "Moving average should be 30.0");
}

void test_lifecycle_phases() {
    auto plan = Plan::make<LifecyclePlan>();

    plan->setup();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();
    TEST_ASSERT(plan->setupDone_, "Setup should have been called");

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();
    TEST_ASSERT(plan->inferDone_, "Infer should have been called");
    TEST_ASSERT(plan->frameCount_ == 1, "Frame count should be 1");

    plan->teardown();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();
    TEST_ASSERT(plan->teardownDone_, "Teardown should have been called");
}

void test_multi_context_execution() {
    auto plan = Plan::make<MultiContextPlan>();
    auto rt = plan->getRuntime();

    struct CustomContext : public PlanContext {
        int execute(const cv::Rect& vp, std::function<void()> fn) override {
            fn();
            return 1;
        }
    };
    rt->registerContext("custom", cv::makePtr<CustomContext>());

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->plainResult_ == 1, "Plain context should set result to 1");
    TEST_ASSERT(plan->customResult_ == 2, "Custom context should set result to 2");
}

void test_edge_dsl_comprehensive() {
    auto plan = Plan::make<EdgeDSLPlan>();

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->sum_ == 13, "10 + 3 = 13");
    TEST_ASSERT(plan->diff_ == 7, "10 - 3 = 7");
    TEST_ASSERT(plan->prod_ == 30, "10 * 3 = 30");
    TEST_ASSERT(plan->quot_ == 3, "10 / 3 = 3");
    TEST_ASSERT(plan->mod_ == 1, "10 % 3 = 1");
    TEST_ASSERT(plan->isGreater_ == true, "10 > 3 is true");
    TEST_ASSERT(plan->maxVal_ == 10, "max(10, 3) = 10");
}

void test_resequence() {
    Resequence reseq(1);
    std::vector<uint64_t> completed;
    std::mutex mtx;

    // Simulate out-of-order completion
    std::thread t1([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        reseq.waitFor(3, [&](uint64_t s) {
            std::lock_guard lock(mtx);
            completed.push_back(s);
        });
    });

    std::thread t2([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        reseq.waitFor(1, [&](uint64_t s) {
            std::lock_guard lock(mtx);
            completed.push_back(s);
        });
    });

    std::thread t3([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        reseq.waitFor(2, [&](uint64_t s) {
            std::lock_guard lock(mtx);
            completed.push_back(s);
        });
    });

    t1.join();
    t2.join();
    t3.join();

    TEST_ASSERT(completed.size() == 3, "All 3 sequences should complete");
    TEST_ASSERT(completed[0] == 1, "First completed should be seq 1");
    TEST_ASSERT(completed[1] == 2, "Second completed should be seq 2");
    TEST_ASSERT(completed[2] == 3, "Third completed should be seq 3");
}

void test_resequence_finish() {
    Resequence reseq(1);
    std::atomic<bool> finished{false};

    std::thread t([&]() {
        reseq.waitFor(99, [&](uint64_t) {
            finished = true;
        });
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    reseq.finish();
    t.join();

    // After finish, waitFor should return without executing callback
    TEST_ASSERT(finished == false, "Callback should not execute after finish()");
}

void test_graph_clearing() {
    auto plan = Plan::make<LifecyclePlan>();

    // Run multiple frames
    for (int i = 0; i < 10; ++i) {
        plan->infer();
        plan->makeGraph();
        plan->runGraph();
        plan->clearGraph();
    }

    TEST_ASSERT(plan->frameCount_ == 10, "Should process 10 frames");
}

int main() {
    std::cout << "=== Plan Integration Tests ===" << std::endl;

    GlobalState::init_keys();
    LocalState::init_keys();

    test_subplan_composition();
    test_lifecycle_phases();
    test_multi_context_execution();
    test_edge_dsl_comprehensive();
    test_resequence();
    test_resequence_finish();
    test_graph_clearing();

    std::cout << "\nResults: " << tests_passed << " passed, " << tests_failed << " failed" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}

