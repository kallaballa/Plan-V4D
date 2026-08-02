// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.

#include "opencv2/plan/plan.hpp"
#include "opencv2/core.hpp"
#include <iostream>
#include <cassert>

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

// Test plan with simple conditional branching
class SimpleBranchPlan : public Plan {
public:
    int value_ = 0;
    int branchATaken_ = 0;
    int branchBTaken_ = 0;
    bool condition_ = false;

    void setCondition(bool c) { condition_ = c; }

    void infer() override {
        branch(R(condition_))
            ->plain([](int& x) { ++x; }, RW(branchATaken_))
        ->elseBranch()
            ->plain([](int& x) { ++x; }, RW(branchBTaken_))
        ->endBranch();
    }
};

// Test plan with value-based branching
class ValueBranchPlan : public Plan {
public:
    int threshold_ = 50;
    int value_ = 0;
    std::string result_;

    void infer() override {
        branch(R(value_) > R(threshold_))
            ->plain([](std::string& r) { r = "high"; }, RW(result_))
        ->elseBranch()
            ->plain([](std::string& r) { r = "low"; }, RW(result_))
        ->endBranch();
    }
};

// Test plan with nested branches
class NestedBranchPlan : public Plan {
public:
    int x_ = 0;
    int y_ = 0;
    std::string path_;

    void infer() override {
        branch(R(x_) > V(0))
            ->branch(R(y_) > V(0))
                ->plain([](std::string& p) { p = "Q1"; }, RW(path_))
            ->elseBranch()
                ->plain([](std::string& p) { p = "Q4"; }, RW(path_))
            ->endBranch()
        ->elseBranch()
            ->branch(R(y_) > V(0))
                ->plain([](std::string& p) { p = "Q2"; }, RW(path_))
            ->elseBranch()
                ->plain([](std::string& p) { p = "Q3"; }, RW(path_))
            ->endBranch()
        ->endBranch();
    }
};

// Test plan with BranchType::ONCE
class OnceBranchPlan : public Plan {
public:
    int onceCounter_ = 0;
    int alwaysCounter_ = 0;

    void infer() override {
        branch(BranchType::ONCE, always_)
            ->plain([](int& c) { ++c; }, RW(onceCounter_))
        ->endBranch();

        plain([](int& c) { ++c; }, RW(alwaysCounter_));
    }
};

// Test plan with BranchType::SINGLE (only one worker executes)
class SingleBranchPlan : public Plan {
public:
    int singleCounter_ = 0;

    void infer() override {
        branch(BranchType::SINGLE, always_)
            ->plain([](int& c) { ++c; }, RW(singleCounter_))
        ->endBranch();
    }
};

// Test plan with edge-based predicate
class EdgePredicatePlan : public Plan {
public:
    int a_ = 10;
    int b_ = 20;
    int maxVal_ = 0;
    int minVal_ = 0;

    void infer() override {
        branch(R(a_) > R(b_))
            ->assign(RW(maxVal_), R(a_))
            ->assign(RW(minVal_), R(b_))
        ->elseBranch()
            ->assign(RW(maxVal_), R(b_))
            ->assign(RW(minVal_), R(a_))
        ->endBranch();
    }
};

void test_simple_branch_true() {
    auto plan = Plan::make<SimpleBranchPlan>();
    plan->setCondition(true);

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->branchATaken_ == 1, "Branch A should be taken when condition is true");
    TEST_ASSERT(plan->branchBTaken_ == 0, "Branch B should not be taken when condition is true");
}

void test_simple_branch_false() {
    auto plan = Plan::make<SimpleBranchPlan>();
    plan->setCondition(false);

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->branchATaken_ == 0, "Branch A should not be taken when condition is false");
    TEST_ASSERT(plan->branchBTaken_ == 1, "Branch B should be taken when condition is false");
}

void test_value_branch_high() {
    auto plan = Plan::make<ValueBranchPlan>();
    plan->value_ = 75;

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->result_ == "high", "Value 75 > 50 should give 'high'");
}

void test_value_branch_low() {
    auto plan = Plan::make<ValueBranchPlan>();
    plan->value_ = 25;

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->result_ == "low", "Value 25 <= 50 should give 'low'");
}

void test_nested_branch_q1() {
    auto plan = Plan::make<NestedBranchPlan>();
    plan->x_ = 5;
    plan->y_ = 5;

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->path_ == "Q1", "(+,+) should be Q1");
}

void test_nested_branch_q2() {
    auto plan = Plan::make<NestedBranchPlan>();
    plan->x_ = -5;
    plan->y_ = 5;

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->path_ == "Q2", "(-,+) should be Q2");
}

void test_nested_branch_q3() {
    auto plan = Plan::make<NestedBranchPlan>();
    plan->x_ = -5;
    plan->y_ = -5;

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->path_ == "Q3", "(-,-) should be Q3");
}

void test_nested_branch_q4() {
    auto plan = Plan::make<NestedBranchPlan>();
    plan->x_ = 5;
    plan->y_ = -5;

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->path_ == "Q4", "(+,-) should be Q4");
}

void test_once_branch() {
    auto plan = Plan::make<OnceBranchPlan>();

    // Run multiple iterations
    for (int i = 0; i < 5; ++i) {
        plan->infer();
        plan->makeGraph();
        plan->runGraph();
        plan->clearGraph();
    }

    TEST_ASSERT(plan->onceCounter_ == 1, "ONCE branch should execute only once");
    TEST_ASSERT(plan->alwaysCounter_ == 5, "Non-branch code should execute every time");
}

void test_edge_predicate() {
    auto plan = Plan::make<EdgePredicatePlan>();

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    TEST_ASSERT(plan->maxVal_ == 20, "max should be 20 (b > a)");
    TEST_ASSERT(plan->minVal_ == 10, "min should be 10 (a < b)");
}

void test_branch_with_computed_predicate() {
    auto plan = Plan::make<ValueBranchPlan>();
    plan->value_ = 50;  // exactly at threshold

    plan->infer();
    plan->makeGraph();
    plan->runGraph();
    plan->clearGraph();

    // 50 > 50 is false, so should take else
    TEST_ASSERT(plan->result_ == "low", "Value 50 == threshold should give 'low' (not strictly greater)");
}

int main() {
    std::cout << "=== Plan Branching Tests ===" << std::endl;

    GlobalState::init_keys();
    LocalState::init_keys();

    test_simple_branch_true();
    test_simple_branch_false();
    test_value_branch_high();
    test_value_branch_low();
    test_nested_branch_q1();
    test_nested_branch_q2();
    test_nested_branch_q3();
    test_nested_branch_q4();
    test_once_branch();
    test_edge_predicate();
    test_branch_with_computed_predicate();

    std::cout << "\nResults: " << tests_passed << " passed, " << tests_failed << " failed" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}

