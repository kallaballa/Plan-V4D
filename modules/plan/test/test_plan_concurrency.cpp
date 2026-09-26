// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
#include "test_precomp.hpp"
#include "MockPlanRuntime.hpp"
#include "TestedPlan.hpp"

#include <atomic>
#include <thread>

namespace opencv_test { namespace {

using namespace cv::plan;
using namespace cv::plan::test;

// A plan that records what it observed about its own run, so that a test can
// tell two concurrent runs apart. Everything it shares with the test is held in a
// shared_ptr: Plan::run instantiates the plan once per participating thread.
struct RunReport {
    // Number of times setup() ran, i.e. the number of worker threads of the run.
    std::atomic<int> setups {0};
    // Number of times a graph node saw itself running on the display thread.
    std::atomic<int> nodesOnDisplayThread {0};
    // Number of times a graph node saw itself running on a worker thread.
    std::atomic<int> nodesOnWorkerThread {0};
    // Identity of the session as seen by setup() and by the frame loop.
    std::atomic<const void*> setupSession {nullptr};
    std::atomic<const void*> frameSession {nullptr};
    // Address of the value behind GlobalState::Keys::FRAME_CNT. Two concurrent
    // runs must not share one property map, so these must differ.
    std::atomic<const void*> frameCntAddress {nullptr};
    std::atomic<int> baseSeen {0};
};

struct ConcurrentPlan : TestedPlan {
    std::shared_ptr<RunReport> report;
    // Written in setup(), read in infer(): proves that a run does not pick up
    // another run's graph.
    int base_ = 3;

    explicit ConcurrentPlan(std::shared_ptr<RunReport> r) : report(std::move(r)) {}

    void setup() override {
        report->setups.fetch_add(1);
        report->setupSession.store(&GlobalState::session());
        assign(RW(base_), V(40));
    }

    void infer() override {
        report->baseSeen.store(base_);
        report->frameSession.store(&GlobalState::session());
        report->frameCntAddress.store(&GlobalState::get<uint64_t>(GlobalState::Keys::FRAME_CNT));
        plain([this]() {
            if(GlobalState::isMain())
                report->nodesOnDisplayThread.fetch_add(1);
            else
                report->nodesOnWorkerThread.fetch_add(1);
        });
    }
};

// Runs one plan with a MockPlanRuntime on a dedicated thread and returns its
// report. extraWorkers is passed to Plan::run, so a plan gets that many worker
// threads plus the thread that started the run.
static std::shared_ptr<RunReport> runPlanOnThread(int extraWorkers, int frames) {
    auto report = std::make_shared<RunReport>();
    std::thread t([report, extraWorkers, frames]() {
        auto rt = cv::makePtr<MockPlanRuntime>(frames);
        PlanRuntime::current() = rt;
        Plan::run<ConcurrentPlan>(extraWorkers, report);
        PlanRuntime::current().reset();
    });
    t.join();
    return report;
}

// ---------- Two plans in two threads ----------

TEST(PlanConcurrencyTest, two_plans_run_concurrently_in_separate_threads) {
    constexpr int frames = 5;
    constexpr int extraWorkers = 1; // 1 worker + the display thread of that plan
    constexpr int workers = extraWorkers + 1;

    std::shared_ptr<RunReport> a, b;
    std::thread ta([&]() { a = runPlanOnThread(extraWorkers, frames); });
    std::thread tb([&]() { b = runPlanOnThread(extraWorkers, frames); });
    ta.join();
    tb.join();

    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    // Both plans ran setup() on every one of their workers and ran every frame.
    EXPECT_EQ(a->setups.load(), workers);
    EXPECT_EQ(b->setups.load(), workers);
    EXPECT_EQ(a->nodesOnWorkerThread.load(), frames * workers);
    EXPECT_EQ(b->nodesOnWorkerThread.load(), frames * workers);

    // The workers of a run share the session of their display thread, and two
    // concurrent runs never share one. (The display thread itself runs no graph
    // node: it displays, the workers infer.)
    EXPECT_EQ(a->setupSession.load(), a->frameSession.load());
    EXPECT_EQ(b->setupSession.load(), b->frameSession.load());
    EXPECT_NE(a->setupSession.load(), nullptr);
    EXPECT_NE(a->setupSession.load(), b->setupSession.load());
    EXPECT_EQ(a->nodesOnDisplayThread.load(), 0);

    // Each run has its own property map, so the value behind FRAME_CNT lives at
    // a different address in the two runs.
    EXPECT_NE(a->frameCntAddress.load(), nullptr);
    EXPECT_NE(a->frameCntAddress.load(), b->frameCntAddress.load());

    // A run sees the state its own setup() produced, never the other's.
    EXPECT_EQ(a->baseSeen.load(), 40);
    EXPECT_EQ(b->baseSeen.load(), 40);
}

// ---------- A plan started from a thread that already displays a plan ----------

// Runtime whose frame loop starts a second plan, i.e. the second plan is started
// from the display thread of the first one.
class NestingRuntime : public MockPlanRuntime {
public:
    std::shared_ptr<RunReport> inner;
    // The inner plan runs with the same runtime (PlanRuntime::current() of this
    // thread), so the frame loop of the inner run must not start a third one.
    std::atomic<bool> started {false};
    NestingRuntime(int frames) : MockPlanRuntime(frames) {}
    void runFrameLoop(std::function<void()> frameFn) override {
        MockPlanRuntime::runFrameLoop(frameFn);
        if(!started.exchange(true))
            Plan::run<ConcurrentPlan>(0, inner);
    }
};

TEST(PlanConcurrencyTest, a_plan_started_from_a_display_thread_gets_its_own_session) {
    constexpr int frames = 2;
    auto outer = std::make_shared<RunReport>();
    auto inner = std::make_shared<RunReport>();

    auto rt = cv::makePtr<NestingRuntime>(frames);
    rt->inner = inner;
    PlanRuntime::current() = rt;
    Plan::run<ConcurrentPlan>(0, outer);
    const void* sessionAfterRun = &GlobalState::session();
    PlanRuntime::current().reset();

    // Both runs completed: the outer run ran its frames before the inner one was
    // started, and the inner one ran its own frames afterwards.
    EXPECT_EQ(outer->setups.load(), 1);
    EXPECT_EQ(inner->setups.load(), 1);
    EXPECT_EQ(outer->nodesOnWorkerThread.load(), frames);
    EXPECT_EQ(inner->nodesOnWorkerThread.load(), frames);
    EXPECT_EQ(outer->baseSeen.load(), 40);
    EXPECT_EQ(inner->baseSeen.load(), 40);

    // The inner run got a session of its own ...
    EXPECT_NE(inner->setupSession.load(), nullptr);
    EXPECT_EQ(inner->setupSession.load(), inner->frameSession.load());
    EXPECT_NE(inner->setupSession.load(), outer->setupSession.load());
    // ... and the thread is not left bound to the inner run: starting a plan from
    // the display thread of another plan does not leak the inner session.
    EXPECT_NE(sessionAfterRun, inner->setupSession.load());
}

// ---------- State outside of a run ----------

TEST(PlanConcurrencyTest, global_state_of_a_finished_run_is_not_leaked_into_the_next_one) {
    GlobalState::init_keys();
    GlobalState::set<uint64_t>(GlobalState::Keys::FRAME_CNT, 17);
    const void* fallbackSession = &GlobalState::session();

    auto report = runPlanOnThread(0, 1);
    EXPECT_EQ(report->setups.load(), 1);
    // The run had a property map of its own ...
    EXPECT_NE(report->frameCntAddress.load(), &GlobalState::get<uint64_t>(GlobalState::Keys::FRAME_CNT));
    // ... and it left the fallback session of this thread untouched.
    EXPECT_EQ(&GlobalState::session(), fallbackSession);
    EXPECT_EQ(GlobalState::get<uint64_t>(GlobalState::Keys::FRAME_CNT), 17u);
}

}} // namespace
