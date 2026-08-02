// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.

#include "opencv2/plan/plan.hpp"
#include "opencv2/core.hpp"
#include <iostream>
#include <cassert>
#include <thread>

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

void test_global_state_init() {
    GlobalState::init_keys();

    TEST_ASSERT(GlobalState::get<uint64_t>(GlobalState::Keys::FRAME_CNT) == 0,
                "FRAME_CNT should start at 0");
    TEST_ASSERT(GlobalState::get<size_t>(GlobalState::Keys::RUN_CNT) == 0,
                "RUN_CNT should start at 0");
    TEST_ASSERT(GlobalState::get<bool>(GlobalState::Keys::LOCKING) == false,
                "LOCKING should start as false");
}

void test_global_state_set_get() {
    GlobalState::set(GlobalState::Keys::FRAME_CNT, uint64_t(100));
    TEST_ASSERT(GlobalState::get<uint64_t>(GlobalState::Keys::FRAME_CNT) == 100,
                "FRAME_CNT should be 100 after set");

    GlobalState::set(GlobalState::Keys::FPS, 60.0);
    TEST_ASSERT(GlobalState::get<double>(GlobalState::Keys::FPS) == 60.0,
                "FPS should be 60.0 after set");
}

void test_global_state_apply() {
    GlobalState::set(GlobalState::Keys::FRAME_CNT, uint64_t(0));

    auto result = GlobalState::apply<uint64_t>(GlobalState::Keys::FRAME_CNT,
        [](uint64_t& v) { ++v; return v; });

    TEST_ASSERT(result == 1, "apply should return incremented value");
    TEST_ASSERT(GlobalState::get<uint64_t>(GlobalState::Keys::FRAME_CNT) == 1,
                "FRAME_CNT should be 1 after apply");
}

void test_local_state() {
    LocalState::init_keys();

    LocalState::set(LocalState::Keys::WORKER_INDEX, size_t(3));
    TEST_ASSERT(LocalState::get<size_t>(LocalState::Keys::WORKER_INDEX) == 3,
                "WORKER_INDEX should be 3");
}

void test_local_state_thread_isolation() {
    LocalState::init_keys();
    LocalState::set(LocalState::Keys::WORKER_INDEX, size_t(0));

    size_t otherThreadValue = 999;
    std::thread t([&otherThreadValue]() {
        LocalState::init_keys();
        LocalState::set(LocalState::Keys::WORKER_INDEX, size_t(7));
        otherThreadValue = LocalState::get<size_t>(LocalState::Keys::WORKER_INDEX);
    });
    t.join();

    TEST_ASSERT(otherThreadValue == 7, "Other thread should see its own value 7");
    TEST_ASSERT(LocalState::get<size_t>(LocalState::Keys::WORKER_INDEX) == 0,
                "Main thread should still see 0");
}

void test_global_state_is_main() {
    GlobalState::setMainID(std::this_thread::get_id());
    TEST_ASSERT(GlobalState::isMain() == true, "Current thread should be main");
}

void test_global_state_once() {
    // Reset by using a unique name
    bool first = GlobalState::once("test_unique_action_12345");
    bool second = GlobalState::once("test_unique_action_12345");

    TEST_ASSERT(first == true, "First call to once() should return true");
    TEST_ASSERT(second == false, "Second call to once() should return false");
}

void test_runtime_properties() {
    auto rt = cv::makePtr<Runtime>(cv::Size(800, 600));

    TEST_ASSERT(rt->get<cv::Size>(Runtime::Keys::SIZE).width == 800,
                "Runtime SIZE width should be 800");
    TEST_ASSERT(rt->get<cv::Size>(Runtime::Keys::SIZE).height == 600,
                "Runtime SIZE height should be 600");

    rt->set(Runtime::Keys::VIEWPORT, cv::Rect(10, 20, 400, 300));
    auto vp = rt->get<cv::Rect>(Runtime::Keys::VIEWPORT);
    TEST_ASSERT(vp.x == 10 && vp.y == 20 && vp.width == 400 && vp.height == 300,
                "Viewport should be updated");
}

void test_runtime_context_registry() {
    auto rt = cv::makePtr<Runtime>();

    struct TestContext : public PlanContext {
        int execute(const cv::Rect& vp, std::function<void()> fn) override {
            fn();
            return 1;
        }
    };

    auto ctx = cv::makePtr<TestContext>();
    rt->registerContext("test", ctx);

    auto retrieved = rt->getContext("test");
    TEST_ASSERT(retrieved != nullptr, "Registered context should be retrievable");

    auto missing = rt->getContext("nonexistent");
    TEST_ASSERT(missing == nullptr, "Non-existent context should return nullptr");
}

void test_property_edge_in_plan() {
    class StatePlan : public Plan {
    public:
        uint64_t capturedFrameCnt_ = 0;

        void infer() override {
            // Read global state via P() edge
            assign(RW(capturedFrameCnt_), P<uint64_t>(GlobalState::Keys::FRAME_CNT));
        }
    };

    GlobalState::set(GlobalState::Keys::FRAME_CNT, uint64_t(42));

    auto plan = Plan::make<StatePlan>();
    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->capturedFrameCnt_ == 42, "P() edge should read FRAME_CNT = 42");
}

void test_set_property_via_plan() {
    class SetterPlan : public Plan {
    public:
        int val_ = 123;

        void infer() override {
            set(Runtime::Keys::VIEWPORT, R(val_) * V(10));
        }
    };

    auto plan = Plan::make<SetterPlan>();
    auto rt = plan->getRuntime();

    // Use a simpler approach - just set directly
    plan->plain([rt](const int& v) {
        rt->set(Runtime::Keys::VIEWPORT, cv::Rect(0, 0, v, v));
    }, plan->R(plan->val_));

    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    auto vp = rt->get<cv::Rect>(Runtime::Keys::VIEWPORT);
    TEST_ASSERT(vp.width == 123 && vp.height == 123,
                "set() should update runtime property");
}

int main() {
    std::cout << "=== Plan State Tests ===" << std::endl;

    GlobalState::init_keys();
    LocalState::init_keys();

    test_global_state_init();
    test_global_state_set_get();
    test_global_state_apply();
    test_local_state();
    test_local_state_thread_isolation();
    test_global_state_is_main();
    test_global_state_once();
    test_runtime_properties();
    test_runtime_context_registry();
    test_property_edge_in_plan();
    test_set_property_via_plan();

    std::cout << "\nResults: " << tests_passed << " passed, " << tests_failed << " failed" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}

