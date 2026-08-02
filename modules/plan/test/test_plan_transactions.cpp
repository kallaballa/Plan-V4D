// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.

#include "opencv2/plan/plan.hpp"
#include "opencv2/core.hpp"
#include "opencv2/imgproc.hpp"
#include <iostream>
#include <cassert>
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

// Test plan that performs simple operations via plain()
class TransactionTestPlan : public Plan {
public:
    int counter_ = 0;
    int result_ = 0;
    double accumulated_ = 0.0;
    std::vector<int> history_;

    void infer() override {
        // Simple increment
        plain([](int& c) { ++c; }, RW(counter_));

        // Assignment via edge
        assign(RW(result_), R(counter_) * V(2));

        // Accumulate
        plain([](double& acc, const int& c) { acc += c; }, RW(accumulated_), R(counter_));

        // Push to history
        plain([](std::vector<int>& h, const int& c) { h.push_back(c); }, RW(history_), R(counter_));
    }
};

// Test plan with multiple sequential operations
class SequentialPlan : public Plan {
public:
    int a_ = 0;
    int b_ = 0;
    int c_ = 0;

    void infer() override {
        plain([](int& x) { x = 10; }, RW(a_));
        plain([](int& x, const int& src) { x = src + 5; }, RW(b_), R(a_));
        plain([](int& x, const int& src) { x = src * 2; }, RW(c_), R(b_));
    }
};

// Test plan that uses F() for function edges
class FunctionEdgePlan : public Plan {
public:
    int x_ = 5;
    int y_ = 3;
    int sum_ = 0;
    int product_ = 0;

    void infer() override {
        // Use F() to compute and assign
        assign(RW(sum_), F([](int a, int b) { return a + b; }, R(x_), R(y_)));
        assign(RW(product_), F([](int a, int b) { return a * b; }, R(x_), R(y_)));
    }
};

// Test plan with UMat operations
class MatTransactionPlan : public Plan {
public:
    cv::UMat input_;
    cv::UMat output_;
    bool processed_ = false;

    void setup() override {
        input_.create(cv::Size(64, 64), CV_8UC3);
        input_.setTo(cv::Scalar(128, 64, 32));
    }

    void infer() override {
        plain([](cv::UMat& out, const cv::UMat& in) {
            cv::cvtColor(in, out, cv::COLOR_BGR2GRAY);
        }, RW(output_), R(input_));

        plain([](bool& done) { done = true; }, RW(processed_));
    }
};

void test_basic_transaction() {
    auto plan = Plan::make<TransactionTestPlan>();
    auto rt = plan->getRuntime();

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->counter_ == 1, "Counter should be incremented to 1");
    TEST_ASSERT(plan->result_ == 2, "Result should be counter * 2 = 2");
    TEST_ASSERT(plan->accumulated_ == 1.0, "Accumulated should be 1.0");
    TEST_ASSERT(plan->history_.size() == 1, "History should have 1 entry");
    TEST_ASSERT(plan->history_[0] == 1, "History[0] should be 1");
}

void test_sequential_transactions() {
    auto plan = Plan::make<SequentialPlan>();
    auto rt = plan->getRuntime();

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->a_ == 10, "a should be 10");
    TEST_ASSERT(plan->b_ == 15, "b should be a + 5 = 15");
    TEST_ASSERT(plan->c_ == 30, "c should be b * 2 = 30");
}

void test_function_edge_transactions() {
    auto plan = Plan::make<FunctionEdgePlan>();
    auto rt = plan->getRuntime();

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->sum_ == 8, "sum should be 5 + 3 = 8");
    TEST_ASSERT(plan->product_ == 15, "product should be 5 * 3 = 15");
}

void test_mat_transactions() {
    auto plan = Plan::make<MatTransactionPlan>();
    auto rt = plan->getRuntime();

    plan->setup();
    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(!plan->output_.empty(), "Output should not be empty");
    TEST_ASSERT(plan->output_.channels() == 1, "Output should be grayscale");
    TEST_ASSERT(plan->output_.size() == cv::Size(64, 64), "Output should be 64x64");
    TEST_ASSERT(plan->processed_ == true, "Processed flag should be true");
}

void test_multiple_iterations() {
    auto plan = Plan::make<TransactionTestPlan>();
    auto rt = plan->getRuntime();

    // Run infer multiple times to simulate frames
    for (int i = 0; i < 5; ++i) {
        plan->infer();
        plan->makeGraph();
        plan->runGraph();
        plan->clearGraph();
    }

    TEST_ASSERT(plan->counter_ == 5, "Counter should be 5 after 5 iterations");
    TEST_ASSERT(plan->result_ == 10, "Result should be 10 after 5 iterations");
    TEST_ASSERT(plan->accumulated_ == 15.0, "Accumulated should be 1+2+3+4+5 = 15");
    TEST_ASSERT(plan->history_.size() == 5, "History should have 5 entries");
}

void test_transaction_with_context() {
    // Test that plain context executes correctly
    auto plan = Plan::make<SequentialPlan>();
    auto rt = plan->getRuntime();

    // Register a custom context
    struct CountingContext : public PlanContext {
        int executeCount = 0;
        int execute(const cv::Rect& vp, std::function<void()> fn) override {
            ++executeCount;
            fn();
            return 1;
        }
    };

    auto countingCtx = cv::makePtr<CountingContext>();
    rt->registerContext("counting", countingCtx);

    // Use the named context
    plan->ctx("counting", [](int& x) { x = 42; }, plan->RW(plan->a_));
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->a_ == 42, "Context execution should set a to 42");
    TEST_ASSERT(countingCtx->executeCount == 1, "Context should have been called once");
}

int main() {
    std::cout << "=== Plan Transaction Tests ===" << std::endl;

    GlobalState::init_keys();
    LocalState::init_keys();

    test_basic_transaction();
    test_sequential_transactions();
    test_function_edge_transactions();
    test_mat_transactions();
    test_multiple_iterations();
    test_transaction_with_context();

    std::cout << "\nResults: " << tests_passed << " passed, " << tests_failed << " failed" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}

