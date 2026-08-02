// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.

#include "opencv2/plan/plan.hpp"
#include "opencv2/core.hpp"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace cv;
using namespace cv::plan;

// A minimal test plan that exposes edge construction for testing
class EdgeTestPlan : public Plan {
public:
    int intVal_ = 42;
    double doubleVal_ = 3.14;
    bool boolVal_ = true;
    cv::UMat matVal_;
    std::string strVal_ = "hello";

    void infer() override {}

    // Expose edge constructors for testing
    auto testR() { return R(intVal_); }
    auto testRW() { return RW(intVal_); }
    auto testV() { return V(99); }
    auto testRS() { return RS(intVal_); }
    auto testRWS() { return RWS(intVal_); }
    auto testCS() { return CS(intVal_); }
    auto testF() { return F([](int a, int b) { return a + b; }, R(intVal_), V(10)); }
};

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

void test_read_edge() {
    auto plan = Plan::make<EdgeTestPlan>();
    auto edge = plan->testR();

    // Read edge should return the value
    TEST_ASSERT(edge.ref() == 42, "R() edge should read value 42");

    // Read edge type traits
    using EdgeType = decltype(edge);
    TEST_ASSERT(EdgeType::read_t::value == true, "R() edge should be read-only");
    TEST_ASSERT(EdgeType::copy_t::value == false, "R() edge should not copy");
    TEST_ASSERT(EdgeType::shared_t::value == false, "R() edge should not be shared");
}

void test_readwrite_edge() {
    auto plan = Plan::make<EdgeTestPlan>();
    auto edge = plan->testRW();

    // RW edge should allow modification
    edge.ref() = 100;
    TEST_ASSERT(plan->intVal_ == 100, "RW() edge should allow writing");

    using EdgeType = decltype(edge);
    TEST_ASSERT(EdgeType::read_t::value == false, "RW() edge should not be read-only");
    TEST_ASSERT(EdgeType::copy_t::value == false, "RW() edge should not copy");
}

void test_value_edge() {
    auto plan = Plan::make<EdgeTestPlan>();
    auto edge = plan->testV();

    TEST_ASSERT(edge.ref() == 99, "V() edge should hold value 99");

    using EdgeType = decltype(edge);
    TEST_ASSERT(EdgeType::read_t::value == true, "V() edge should be read-only");
    TEST_ASSERT(EdgeType::byvalue_t::value == true, "V() edge should be by-value");
}

void test_shared_read_edge() {
    auto plan = Plan::make<EdgeTestPlan>();
    plan->_shared(plan->intVal_);
    auto edge = plan->testRS();

    TEST_ASSERT(edge.ref() == 42, "RS() edge should read shared value");

    using EdgeType = decltype(edge);
    TEST_ASSERT(EdgeType::shared_t::value == true, "RS() edge should be shared");
    TEST_ASSERT(EdgeType::read_t::value == true, "RS() edge should be read-only");
}

void test_shared_readwrite_edge() {
    auto plan = Plan::make<EdgeTestPlan>();
    plan->_shared(plan->intVal_);
    auto edge = plan->testRWS();

    edge.ref() = 200;
    TEST_ASSERT(plan->intVal_ == 200, "RWS() edge should allow writing to shared var");

    using EdgeType = decltype(edge);
    TEST_ASSERT(EdgeType::shared_t::value == true, "RWS() edge should be shared");
    TEST_ASSERT(EdgeType::read_t::value == false, "RWS() edge should not be read-only");
    TEST_ASSERT(EdgeType::lockie_t::value == true, "RWS() edge should be a lockie");
}

void test_function_edge() {
    auto plan = Plan::make<EdgeTestPlan>();
    auto edge = plan->testF();

    // F() creates an edge that evaluates a function
    TEST_ASSERT(edge.ref() == 52, "F() edge should evaluate 42 + 10 = 52");
}

void test_edge_arithmetic_operators() {
    auto plan = Plan::make<EdgeTestPlan>();

    // Test addition
    auto sumEdge = plan->R(plan->intVal_) + plan->V(8);
    // The operator returns a Plan ptr (deferred), so we test via graph execution
    TEST_ASSERT(sumEdge.ptr() != nullptr, "operator+ should return non-null");

    // Test subtraction
    auto diffEdge = plan->R(plan->intVal_) - plan->V(2);
    TEST_ASSERT(diffEdge.ptr() != nullptr, "operator- should return non-null");

    // Test multiplication
    auto mulEdge = plan->R(plan->intVal_) * plan->V(2);
    TEST_ASSERT(mulEdge.ptr() != nullptr, "operator* should return non-null");

    // Test division
    auto divEdge = plan->R(plan->intVal_) / plan->V(2);
    TEST_ASSERT(divEdge.ptr() != nullptr, "operator/ should return non-null");
}

void test_edge_comparison_operators() {
    auto plan = Plan::make<EdgeTestPlan>();

    auto eqEdge = plan->R(plan->intVal_) == plan->V(42);
    TEST_ASSERT(eqEdge.ptr() != nullptr, "operator== should return non-null");

    auto neqEdge = plan->R(plan->intVal_) != plan->V(0);
    TEST_ASSERT(neqEdge.ptr() != nullptr, "operator!= should return non-null");

    auto ltEdge = plan->R(plan->intVal_) < plan->V(100);
    TEST_ASSERT(ltEdge.ptr() != nullptr, "operator< should return non-null");

    auto gtEdge = plan->R(plan->intVal_) > plan->V(0);
    TEST_ASSERT(gtEdge.ptr() != nullptr, "operator> should return non-null");

    auto leEdge = plan->R(plan->intVal_) <= plan->V(42);
    TEST_ASSERT(leEdge.ptr() != nullptr, "operator<= should return non-null");

    auto geEdge = plan->R(plan->intVal_) >= plan->V(42);
    TEST_ASSERT(geEdge.ptr() != nullptr, "operator>= should return non-null");
}

void test_edge_logical_operators() {
    auto plan = Plan::make<EdgeTestPlan>();

    auto andEdge = plan->R(plan->boolVal_) && plan->V(true);
    TEST_ASSERT(andEdge.ptr() != nullptr, "operator&& should return non-null");

    auto orEdge = plan->R(plan->boolVal_) || plan->V(false);
    TEST_ASSERT(orEdge.ptr() != nullptr, "operator|| should return non-null");

    auto notEdge = !plan->R(plan->boolVal_);
    TEST_ASSERT(notEdge.ptr() != nullptr, "operator! should return non-null");
}

void test_edge_bitwise_operators() {
    auto plan = Plan::make<EdgeTestPlan>();

    auto xorEdge = plan->R(plan->intVal_) ^ plan->V(0xFF);
    TEST_ASSERT(xorEdge.ptr() != nullptr, "operator^ should return non-null");

    auto bandEdge = plan->R(plan->intVal_) & plan->V(0x0F);
    TEST_ASSERT(bandEdge.ptr() != nullptr, "operator& should return non-null");

    auto borEdge = plan->R(plan->intVal_) | plan->V(0xF0);
    TEST_ASSERT(borEdge.ptr() != nullptr, "operator| should return non-null");

    auto shlEdge = plan->R(plan->intVal_) << plan->V(2);
    TEST_ASSERT(shlEdge.ptr() != nullptr, "operator<< should return non-null");

    auto shrEdge = plan->R(plan->intVal_) >> plan->V(2);
    TEST_ASSERT(shrEdge.ptr() != nullptr, "operator>> should return non-null");
}

void test_edge_modulo() {
    auto plan = Plan::make<EdgeTestPlan>();
    auto modEdge = plan->R(plan->intVal_) % plan->V(5);
    TEST_ASSERT(modEdge.ptr() != nullptr, "operator% should return non-null");
}

void test_edge_tuple_syntax() {
    auto plan = Plan::make<EdgeTestPlan>();

    // Test the _() tuple helper
    auto tup = plan->_(plan->R(plan->intVal_), plan->V(10));
    TEST_ASSERT(std::tuple_size<decltype(tup)>::value == 2, "_() should create a 2-tuple");

    auto tup3 = plan->_(plan->R(plan->intVal_), plan->V(10), plan->V(20));
    TEST_ASSERT(std::tuple_size<decltype(tup3)>::value == 3, "_() should create a 3-tuple");
}

void test_property_edge() {
    auto plan = Plan::make<EdgeTestPlan>();
    auto rt = plan->getRuntime();

    // Test P() with Runtime keys
    auto sizeProp = plan->P<cv::Size>(Runtime::Keys::SIZE);
    TEST_ASSERT(sizeProp.ref().width == 1920, "P(SIZE) should read runtime size width");
    TEST_ASSERT(sizeProp.ref().height == 1080, "P(SIZE) should read runtime size height");
}

int main() {
    std::cout << "=== Plan Edge Tests ===" << std::endl;

    GlobalState::init_keys();
    LocalState::init_keys();

    test_read_edge();
    test_readwrite_edge();
    test_value_edge();
    test_shared_read_edge();
    test_shared_readwrite_edge();
    test_function_edge();
    test_edge_arithmetic_operators();
    test_edge_comparison_operators();
    test_edge_logical_operators();
    test_edge_bitwise_operators();
    test_edge_modulo();
    test_edge_tuple_syntax();
    test_property_edge();

    std::cout << "\nResults: " << tests_passed << " passed, " << tests_failed << " failed" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}

