// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Shared State Demo
// Demonstrates: shared variable declaration, RWS/RS/CS edges,
//               thread-safe access patterns, and the GlobalState machine.

#include <opencv2/plan/plan.hpp>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <numeric>

using namespace cv;
using namespace cv::plan;

/*!
 * A plan that demonstrates shared state access patterns.
 * Multiple "workers" (simulated by sequential graph runs) access
 * shared counters with proper locking.
 */
class SharedStatePlan : public Plan {
public:
    // Shared state (declared in constructor)
    int sharedCounter_ = 0;
    double sharedAccumulator_ = 0.0;
    cv::UMat sharedFrame_;

    // Per-worker state
    int workerId_ = 0;
    int localCount_ = 0;

    SharedStatePlan() {
        _shared(sharedCounter_);
        _shared(sharedAccumulator_);
        _shared(sharedFrame_);
    }

    void setup() override {
        sharedFrame_.create(cv::Size(64, 64), CV_8UC1);
        sharedFrame_.setTo(cv::Scalar(0));
    }

    void infer() override {
        // RWS: Read-Write-Shared - locks the mutex during execution
        plain([](int& counter, const int& workerId) {
            ++counter;
        }, RWS(sharedCounter_), R(workerId_));

        // RWS on accumulator
        plain([](double& acc, const int& workerId) {
            acc += workerId * 0.1;
        }, RWS(sharedAccumulator_), R(workerId_));

        // RS: Read-Shared - reads under lock
        plain([](int& local, const int& shared) {
            local = shared;
        }, RW(localCount_), RS(sharedCounter_));

        // CS: Copy-Shared - creates a safe copy
        plain([](cv::UMat& localCopy, const cv::UMat& sharedFrame) {
            localCopy = sharedFrame.clone();
        }, RW(localFrameCopy_), CS(sharedFrame_));
    }

    cv::UMat localFrameCopy_;
};

/*!
 * Demonstrates GlobalState as a shared communication channel.
 */
class GlobalStatePlan : public Plan {
public:
    uint64_t capturedFrameCnt_ = 0;
    double capturedFps_ = 0.0;

    void infer() override {
        // Read from GlobalState via P() edges
        assign(RW(capturedFrameCnt_), P<uint64_t>(GlobalState::Keys::FRAME_CNT));
        assign(RW(capturedFps_), P<double>(GlobalState::Keys::FPS));
    }
};

int main() {
    std::cout << "=== Plan Module: Shared State Demo ===" << std::endl;
    std::cout << std::endl;

    GlobalState::init_keys();
    LocalState::init_keys();

    // --- Part 1: Shared Variables with Locking ---
    std::cout << "--- Part 1: Shared Variables with Locking ---" << std::endl;

    auto plan = Plan::make<SharedStatePlan>();
    plan->setup();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    // Simulate 4 workers each doing 10 iterations
    constexpr int NUM_WORKERS = 4;
    constexpr int ITERATIONS = 10;

    for (int w = 0; w < NUM_WORKERS; ++w) {
        plan->workerId_ = w + 1;
        for (int i = 0; i < ITERATIONS; ++i) {
            plan->infer();
            plan->makeGraph();
            plan->runGraph();
            plan->clearGraph();
        }
    }

    std::cout << "  Shared counter: " << plan->sharedCounter_
              << " (expected: " << NUM_WORKERS * ITERATIONS << ")" << std::endl;
    std::cout << "  Shared accumulator: " << std::fixed << std::setprecision(1)
              << plan->sharedAccumulator_ << std::endl;
    std::cout << "  Last worker local count: " << plan->localCount_ << std::endl;
    std::cout << "  Shared frame size: " << plan->sharedFrame_.size() << std::endl;

    // --- Part 2: Concurrent Access Simulation ---
    std::cout << "\n--- Part 2: Concurrent Access (std::thread) ---" << std::endl;

    SharedVariables sv;
    int concurrentCounter = 0;
    sv.makeSharedVar(concurrentCounter);

    constexpr int THREAD_COUNT = 8;
    constexpr int OPS_PER_THREAD = 10000;

    std::vector<std::thread> threads;
    auto start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < THREAD_COUNT; ++t) {
        threads.emplace_back([&sv, &concurrentCounter]() {
            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                sv.lock(concurrentCounter);
                ++concurrentCounter;
                sv.unlock(concurrentCounter);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "  " << THREAD_COUNT << " threads x " << OPS_PER_THREAD << " ops" << std::endl;
    std::cout << "  Result: " << concurrentCounter
              << " (expected: " << THREAD_COUNT * OPS_PER_THREAD << ")" << std::endl;
    std::cout << "  Time: " << elapsed.count() << " ms" << std::endl;
    std::cout << "  Correct: " << (concurrentCounter == THREAD_COUNT * OPS_PER_THREAD ? "YES" : "NO")
              << std::endl;

    // --- Part 3: GlobalState Communication ---
    std::cout << "\n--- Part 3: GlobalState Communication ---" << std::endl;

    GlobalState::set(GlobalState::Keys::FRAME_CNT, uint64_t(1234));
    GlobalState::set(GlobalState::Keys::FPS, 59.94);

    auto gsPlan = Plan::make<GlobalStatePlan>();
    gsPlan->infer();
    gsPlan->makeGraph();
    gsPlan->runGraph();
    gsPlan->clearGraph();

    std::cout << "  Captured FRAME_CNT: " << gsPlan->capturedFrameCnt_ << std::endl;
    std::cout << "  Captured FPS: " << gsPlan->capturedFps_ << std::endl;

    std::cout << "\nDone." << std::endl;
    return 0;
}

