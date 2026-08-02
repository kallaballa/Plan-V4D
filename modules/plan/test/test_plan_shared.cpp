// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.

#include "opencv2/plan/plan.hpp"
#include "opencv2/core.hpp"
#include <iostream>
#include <cassert>
#include <thread>
#include <atomic>

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

void test_shared_var_registration() {
    SharedVariables sv;
    int sharedVal = 42;

    sv.makeSharedVar(sharedVal);

    // Should be able to get mutex
    auto* mtx = sv.getMutexPtr(sharedVal);
    TEST_ASSERT(mtx != nullptr, "Shared var should have a mutex");

    // Should be lockable
    mtx->lock();
    mtx->unlock();
    TEST_ASSERT(true, "Shared var mutex should be lockable/unlockable");
}

void test_shared_var_lock_unlock() {
    SharedVariables sv;
    int val = 0;
    sv.makeSharedVar(val);

    sv.lock(val);
    val = 100;
    sv.unlock(val);

    TEST_ASSERT(val == 100, "Value should be modified under lock");
}

void test_shared_var_try_lock() {
    SharedVariables sv;
    int val = 0;
    sv.makeSharedVar(val);

    bool locked = sv.tryLock(val);
    TEST_ASSERT(locked == true, "tryLock should succeed on unlocked var");

    if (locked) {
        sv.unlock(val);
    }
}

void test_shared_var_safe_copy() {
    SharedVariables sv;
    int src = 42;
    int dst = 0;
    sv.makeSharedVar(src);
    sv.makeSharedVar(dst);

    sv.safe_copy(src, dst);
    TEST_ASSERT(dst == 42, "safe_copy should copy value under lock");
}

void test_shared_var_mat_copy() {
    SharedVariables sv;
    cv::UMat src(cv::Size(32, 32), CV_8UC3, cv::Scalar(255, 128, 64));
    cv::UMat dst;
    sv.makeSharedVar(src);
    sv.makeSharedVar(dst);

    sv.safe_copy(src, dst);
    TEST_ASSERT(!dst.empty(), "safe_copy should copy UMat");
    TEST_ASSERT(dst.size() == src.size(), "Copied UMat should have same size");
}

void test_non_shared_var_throws() {
    SharedVariables sv;
    int nonShared = 0;

    bool threw = false;
    try {
        sv.getMutexPtr(nonShared, true);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    TEST_ASSERT(threw, "getMutexPtr on non-shared var should throw");
}

void test_safe_var_registration() {
    SharedVariables sv;
    int safeVal = 0;
    sv.registerSafe(safeVal);

    // Safe vars should not be treated as shared
    // checkShared should return false for safe vars
    class DummyPlan : public Plan {
    public:
        void infer() override {}
    };
    auto plan = Plan::make<DummyPlan>();

    bool isShared = sv.checkShared(*plan, safeVal);
    TEST_ASSERT(isShared == false, "Safe var should not be reported as shared");
}

void test_concurrent_shared_access() {
    SharedVariables sv;
    int counter = 0;
    sv.makeSharedVar(counter);

    constexpr int NUM_THREADS = 4;
    constexpr int INCREMENTS = 1000;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&sv, &counter, &errors]() {
            for (int i = 0; i < INCREMENTS; ++i) {
                sv.lock(counter);
                ++counter;
                sv.unlock(counter);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    TEST_ASSERT(counter == NUM_THREADS * INCREMENTS,
                "Concurrent increments under lock should be exact");
}

void test_plan_shared_edges() {
    class SharedPlan : public Plan {
    public:
        int sharedCounter_ = 0;

        SharedPlan() {
            _shared(sharedCounter_);
        }

        void infer() override {
            // RWS on shared var - should lock during execution
            plain([](int& c) { ++c; }, RWS(sharedCounter_));
        }
    };

    auto plan = Plan::make<SharedPlan>();

    for (int i = 0; i < 10; ++i) {
        plan->infer();
        plan->makeGraph();
        plan->runGraph();
        plan->clearGraph();
    }

    TEST_ASSERT(plan->sharedCounter_ == 10,
                "Shared counter should be 10 after 10 iterations");
}

void test_plan_copy_shared_edge() {
    class CopyPlan : public Plan {
    public:
        int sharedVal_ = 42;
        int localCopy_ = 0;

        CopyPlan() {
            _shared(sharedVal_);
        }

        void infer() override {
            // CS creates a safe copy
            plain([](int& dst, const int& src) { dst = src; },
                  RW(localCopy_), CS(sharedVal_));
        }
    };

    auto plan = Plan::make<CopyPlan>();
    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->localCopy_ == 42, "CS copy should get value 42");

    // Modify original - copy should not change
    plan->sharedVal_ = 100;
    TEST_ASSERT(plan->localCopy_ == 42, "Local copy should not change when original changes");
}

int main() {
    std::cout << "=== Plan Shared Variable Tests ===" << std::endl;

    GlobalState::init_keys();
    LocalState::init_keys();

    test_shared_var_registration();
    test_shared_var_lock_unlock();
    test_shared_var_try_lock();
    test_shared_var_safe_copy();
    test_shared_var_mat_copy();
    test_non_shared_var_throws();
    test_safe_var_registration();
    test_concurrent_shared_access();
    test_plan_shared_edges();
    test_plan_copy_shared_edge();

    std::cout << "\nResults: " << tests_passed << " passed, " << tests_failed << " failed" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}

