#include "opencv2/plan/plan.hpp"
#include <opencv2/core/utils/logger.hpp>
#include <algorithm>
#include <sstream>

namespace cv {
namespace plan {

static const cv::utils::logging::LogTag plan_tag("Plan", cv::utils::logging::LogLevel::LOG_LEVEL_INFO);

// --- Runtime ---
Runtime::Runtime(const cv::Size& size) {
    plainContext_ = new PlainContext();
    init_keys(size);
}

Runtime::~Runtime() {}

// --- Plan ---
Plan::Plan() {
    runtime_ = new Runtime();
}

Plan::~Plan() {
    self_ = nullptr;
}

void Plan::makeGraph() {
    for(const auto& t : accesses_) {
        const string& name = std::get<0>(t);
        const bool& read = std::get<1>(t);
        const size_t& dep = std::get<2>(t);

        cv::Ptr<Node> n;
        findNode(name, n);
        if(!n) {
            n = new Node();
            n->name_ = name;
            n->tx_ = transactions_[name];
            CV_Assert(!n->name_.empty());
            CV_Assert(n->tx_);
            currentNodes_.push_back(n);
        }

        if(read) {
            n->read_deps_.insert(dep);
        } else {
            n->write_deps_.insert(dep);
        }
    }
}

void Plan::runGraph() {
    BranchType::Enum btype;
    BranchState currentState;

    try {
        for (auto& n : currentNodes_) {
            btype = n->tx_->getBranchType();
            bool isBranch = n->name_.substr(0, 6) == "branch";
            bool isElse = n->name_.substr(0, 6) == "[else]";
            bool isEnd = n->name_.substr(0, 5) == "[end]";

            if(btype != BranchType::NONE) {
                CV_Assert((((isBranch != isElse) != isEnd)));

                if(isBranch) {
                    if(!branchStateStack_.empty())
                        currentState = branchStateStack_.front();
                    else
                        currentState = BranchState();

                    currentState.branchID_ = n->name_;

                    if(currentState.isEnabled_) {
                        currentState.isOnce_ = ((btype == BranchType::ONCE) || (btype == BranchType::PARALLEL_ONCE));
                        currentState.isSingle_ = ((btype == BranchType::ONCE) || (btype == BranchType::SINGLE));
                    } else {
                        currentState.isOnce_ = false;
                        currentState.isSingle_ = false;
                        currentState.isEnabled_ = false;
                    }

                    if(currentState.isEnabled_) {
                        if(currentState.isOnce_) {
                            if(btype == BranchType::ONCE) {
                                currentState.condition_ = GlobalState::once(n->name_) && n->tx_->performPredicate();
                            } else if(btype == BranchType::PARALLEL_ONCE) {
                                currentState.condition_ = !n->tx_->ran() && n->tx_->performPredicate();
                            }
                        } else {
                            currentState.condition_ = n->tx_->performPredicate();
                        }
                        currentState.isEnabled_ = currentState.isEnabled_ && currentState.condition_;

                        if(currentState.isEnabled_ && currentState.isSingle_) {
                            GlobalState::lockNode(currentState.branchID_);
                            currentState.isLocked_ = true;
                        }
                    }
                    branchStateStack_.push_front(currentState);
                } else if(isElse) {
                    if(branchStateStack_.empty())
                        continue;
                    currentState = branchStateStack_.front();
                    currentState.isEnabled_ = !currentState.condition_;
                    currentState.isOnce_ = false;
                    currentState.condition_ = !currentState.condition_;
                    currentState.isSingle_ = false;
                    if(currentState.isLocked_) {
                        GlobalState::tryUnlockNode(currentState.branchID_);
                    }
                    currentState.isLocked_ = false;
                    branchStateStack_.pop_front();
                    branchStateStack_.push_front(currentState);
                } else if(isEnd) {
                    if(branchStateStack_.empty())
                        continue;
                    currentState = branchStateStack_.front();
                    GlobalState::tryUnlockNode(currentState.branchID_);
                    branchStateStack_.pop_front();
                }
            } else {
                CV_Assert(!n->tx_->isPredicate());
                currentState = !branchStateStack_.empty() ? branchStateStack_.front() : BranchState();

                if(currentState.isEnabled_) {
                    auto lock = GlobalState::tryGetNodeLock(currentState.branchID_);
                    auto plan = self<Plan>();

                    if(lock) {
                        std::lock_guard<std::mutex> guard(*lock.get());
                        auto ctx = n->tx_->getContextCallback()();
                        auto viewport = runtime_->get<cv::Rect>(Runtime::Keys::VIEWPORT);
                        int res = ctx->execute(viewport, [plan, n]() {
                            n->tx_->perform();
                        });
                        if(res <= 0) {
                            CV_LOG_WARNING(&plan_tag, "Context failed while: " + n->name_);
                        }
                    } else {
                        auto ctx = n->tx_->getContextCallback()();
                        auto viewport = runtime_->get<cv::Rect>(Runtime::Keys::VIEWPORT);
                        int res = ctx->execute(viewport, [plan, n]() {
                            n->tx_->perform();
                        });
                        if(res <= 0) {
                            CV_LOG_WARNING(&plan_tag, "Context failed while: " + n->name_);
                        }
                    }
                }
                currentState = BranchState();
            }
        }

        size_t lockCnt = GlobalState::countNodeLocks();
        CV_Assert(branchStateStack_.empty());
        CV_Assert(lockCnt == 0);
    } catch(std::runtime_error& ex) {
        if(!branchStateStack_.empty() && branchStateStack_.front().isLocked_) {
            GlobalState::tryUnlockNode(currentState.branchID_);
        }
        throw ex;
    } catch(std::exception& ex) {
        if(!branchStateStack_.empty() && branchStateStack_.front().isLocked_) {
            GlobalState::tryUnlockNode(currentState.branchID_);
        }
        throw ex;
    } catch(...) {
        if(!branchStateStack_.empty() && branchStateStack_.front().isLocked_) {
            GlobalState::tryUnlockNode(currentState.branchID_);
        }
        throw std::runtime_error("Unknown error.");
    }
}

void Plan::clearGraph() {
    std::copy(currentNodes_.begin(), currentNodes_.end(), std::back_inserter(allNodes_));
    accesses_.clear();
    branchStateStack_.clear();
    branchStack_.clear();
    transactions_.clear();
    currentNodes_.clear();
}

template<typename Tplan, typename ... Args>
void Plan::run(int32_t workers, cv::Ptr<Runtime> runtime, Args&& ... args) {
    CV_Assert(workers >= 0);
    ++workers; // +1 for main thread

    cv::Ptr<Tplan> plan;
    static std::mutex worker_init_mtx_;
    std::vector<std::thread*> threads;

    {
        static std::mutex runMtx;
        std::lock_guard<std::mutex> lock(runMtx);
        cv::setNumThreads(0);

        if(GlobalState::isFirstRun()) {
            GlobalState::setMainID(std::this_thread::get_id());
            CV_LOG_INFO(&plan_tag, "Starting with " << workers << " workers");
        }

        plan = make<Tplan>(std::forward<Args>(args)...);
        plan->setRuntime(runtime);

        if(GlobalState::isMain()) {
            GlobalState::set<size_t>(GlobalState::Keys::WORKERS_STARTED, workers);
            for (int32_t i = 0; i < workers; ++i) {
                threads.push_back(new std::thread([plan, runtime, i, &args...] {
                    string name = "plan-worker-" + std::to_string(i);
                    setThreadName(name.c_str());
                    LocalState::set(LocalState::Keys::WORKER_INDEX, size_t(i));
                    Plan::run<Tplan>(0, runtime, std::forward<Args>(args)...);
                }));
            }
        }
    }

    CV_Assert(plan);

    if(!GlobalState::isMain()) {
        static std::binary_semaphore setup_sema(1);
        try {
            setup_sema.acquire();
            plan->setup();
            plan->makeGraph();
            plan->runGraph();
            plan->clearGraph();
            setup_sema.release();
        } catch(std::exception& ex) {
            CV_Error_(cv::Error::StsError, ("Setup failed: %s", ex.what()));
        }
    }

    if(GlobalState::isMain()) {
        CV_LOG_INFO(&plan_tag, "Starting pipelines with " << GlobalState::get<size_t>(GlobalState::Keys::WORKERS_STARTED) << " workers.");
    } else {
        try {
            plan->infer();
            plan->makeGraph();
        } catch(std::exception& ex) {
            CV_Error_(cv::Error::StsError, ("Main inference failed: %s", ex.what()));
        }
        GlobalState::apply<size_t>(GlobalState::Keys::WORKERS_READY, [](size_t& wr){ ++wr; return wr; });
    }

    static std::barrier syncPoint(std::ptrdiff_t(workers + 1));
    syncPoint.arrive_and_wait();

    // Main execution loop
    static Resequence reseq(1);
    try {
        while(true) {
            GlobalState::apply<size_t>(GlobalState::Keys::RUN_CNT, [](size_t& s) { ++s; return s; });
            size_t seq = GlobalState::apply<size_t>(GlobalState::Keys::FRAME_CNT, [](size_t& s) { ++s; return s; });

            plan->runGraph();

            reseq.waitFor(seq, [](uint64_t s) { CV_UNUSED(s); });

            // Check if we should stop (user can call request_finish or set a flag)
            // For now, run indefinitely until exception or external signal
        }
    } catch(std::runtime_error& ex) {
        CV_LOG_WARNING(&plan_tag, "Pipeline terminated: " << ex.what());
    } catch(std::exception& ex) {
        CV_LOG_WARNING(&plan_tag, "Pipeline terminated: " << ex.what());
    }

    reseq.finish();

    if(!GlobalState::isMain()) {
        plan->clearGraph();
        try {
            plan->teardown();
            plan->makeGraph();
            plan->runGraph();
            plan->clearGraph();
        } catch(std::exception& ex) {
            CV_Error_(cv::Error::StsError, ("Pipeline teardown failed: %s", ex.what()));
        }
    } else {
        for(auto& t : threads)
            t->join();
        CV_LOG_INFO(&plan_tag, "All threads terminated.");
    }
}

} // namespace plan
} // namespace cv

