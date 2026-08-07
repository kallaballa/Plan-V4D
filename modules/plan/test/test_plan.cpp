// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
#include "test_precomp.hpp"

namespace cv {
namespace plan {
namespace test {

struct SmokePlan : public Plan {
    int& frameCounter;

    explicit SmokePlan(int& c) : frameCounter(c) {}

    void infer() override {
        plain([this](const uint64_t& frame) {
            if(frame >= 10)
                request_finish();
        }, P<uint64_t>(GlobalState::Keys::FRAME_CNT));
        plain([this]() {
            ++frameCounter;
        });
    }
};

TEST(Plan, SmokeRun) {
    reset_finish();
    Runtime::init("plan-smoke");
    int frameCounter = 0;
    auto arg = std::ref(frameCounter);
    SmokePlan::run<SmokePlan>(0, arg);
    EXPECT_GE(frameCounter, 10);
}

struct MathPlan : public Plan {
    int& result;

    explicit MathPlan(int& r) : result(r) {}

    void infer() override {
        auto a = V(6);
        auto b = V(7);
        plain([this](const int& v, const uint64_t& frame) {
            result = v;
            if(frame >= 3)
                request_finish();
        }, MUL(a, b), P<uint64_t>(GlobalState::Keys::FRAME_CNT));
    }
};

TEST(Plan, Operators) {
    reset_finish();
    Runtime::init("plan-math");
    int result = 0;
    auto arg = std::ref(result);
    MathPlan::run<MathPlan>(0, arg);
    EXPECT_EQ(result, 42);
}

struct BranchPlan : public Plan {
    int& taken;

    explicit BranchPlan(int& t) : taken(t) {}

    void infer() override {
        branch(Plan::always_);
        plain([this](const uint64_t& frame) {
            ++taken;
            if(frame >= 3)
                request_finish();
        }, P<uint64_t>(GlobalState::Keys::FRAME_CNT));
        endBranch();
    }
};

TEST(Plan, Branching) {
    reset_finish();
    Runtime::init("plan-branch");
    int taken = 0;
    auto arg = std::ref(taken);
    BranchPlan::run<BranchPlan>(0, arg);
    EXPECT_GE(taken, 3);
}

} /* namespace test */
} /* namespace plan */
} /* namespace cv */
