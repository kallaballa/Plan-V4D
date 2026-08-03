// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
#include "opencv2/plan/plan.hpp"
#include <algorithm>
#include <iostream>

namespace plan {

void Plan::makeGraph() {
    for (const auto& t : accesses_) {
        const string& name = std::get<0>(t);
        const bool& read   = std::get<1>(t);
        const size_t& dep  = std::get<2>(t);
        Ptr<Node> n;
        findNode(name, n);
        if (!n) {
            n = makePtr<Node>();
            n->name_ = name;
            n->tx_ = transactions_[name];
            PLAN_Assert(!n->name_.empty());
            PLAN_Assert(n->tx_);
            currentNodes_.push_back(n);
        }
        if (read) n->read_deps_.insert(dep);
        else      n->write_deps_.insert(dep);
    }
}

void Plan::pf(const size_t& depth, const BranchState& current, const Ptr<Node> n) {
    PLAN_UNUSED(depth);
    PLAN_UNUSED(current);
    PLAN_UNUSED(n);
}

void Plan::runGraph() {
    BranchType::Enum btype;
    BranchState currentState;

    try {
        for (auto& n : currentNodes_) {
            btype = n->tx_->getBranchType();
            bool isBranch = n->name_.substr(0, 6) == "branch";
            bool isElse   = n->name_.substr(0, 6) == "[else]";
            bool isEnd    = n->name_.substr(0, 5) == "[end]";

            if (btype != BranchType::NONE) {
                PLAN_Assert(((isBranch != isElse) != isEnd));

                if (isBranch) {
                    if (!branchStateStack_.empty())
                        currentState = branchStateStack_.front();
                    else
                        currentState = BranchState();
                    currentState.branchID_ = n->name_;
                    if (currentState.isEnabled_) {
                        currentState.isOnce_ =
                            (btype == BranchType::ONCE) || (btype == BranchType::PARALLEL_ONCE);
                        currentState.isSingle_ =
                            (btype == BranchType::ONCE) || (btype == BranchType::SINGLE);
                    } else {
                        currentState.isOnce_ = false;
                        currentState.isSingle_ = false;
                        currentState.isEnabled_ = false;
                    }
                    if (currentState.isEnabled_) {
                        if (currentState.isOnce_) {
                            if (btype == BranchType::ONCE)
                                currentState.condition_ =
                                    GlobalState::once(n->name_) && n->tx_->performPredicate();
                            else if (btype == BranchType::PARALLEL_ONCE)
                                currentState.condition_ =
                                    !n->tx_->ran() && n->tx_->performPredicate();
                            else
                                PLAN_Assert(false);
                        } else {
                            currentState.condition_ = n->tx_->performPredicate();
                        }
                        currentState.isEnabled_ = currentState.isEnabled_ && currentState.condition_;
                        if (currentState.isEnabled_ && currentState.isSingle_) {
                            PLAN_Assert(btype != BranchType::PARALLEL);
                            GlobalState::lockNode(currentState.branchID_);
                            currentState.isLocked_ = true;
                        }
                    }
                    branchStateStack_.push_front(currentState);
                } else if (isElse) {
                    if (branchStateStack_.empty()) continue;
                    currentState = branchStateStack_.front();
                    currentState.isEnabled_ = !currentState.condition_;
                    currentState.isOnce_ = false;
                    currentState.condition_ = !currentState.condition_;
                    currentState.isSingle_ = false;
                    if (currentState.isLocked_)
                        GlobalState::tryUnlockNode(currentState.branchID_);
                    currentState.isLocked_ = false;
                    branchStateStack_.pop_front();
                    branchStateStack_.push_front(currentState);
                } else if (isEnd) {
                    if (branchStateStack_.empty()) continue;
                    currentState = branchStateStack_.front();
                    GlobalState::tryUnlockNode(currentState.branchID_);
                    branchStateStack_.pop_front();
                } else {
                    PLAN_Assert(false);
                }
            } else {
                PLAN_Assert(!n->tx_->isPredicate());
                currentState = !branchStateStack_.empty()
                    ? branchStateStack_.front() : BranchState();

                if (currentState.isEnabled_) {
                    auto lock = GlobalState::tryGetNodeLock(currentState.branchID_);
                    auto plan = self<Plan>();
                    auto ctx = n->tx_->getContextCallback()();
                    auto viewport = runtime_->viewport();

                    if (lock) {
                        std::lock_guard<std::mutex> guard(*lock.get());
                        int res = ctx->execute(viewport, [plan, n, currentState]() {
                            n->tx_->perform();
                        });
                        if (res <= 0)
                            std::cerr << "[WARN] Context failed while: " << n->name_ << std::endl;
                    } else {
                        int res = ctx->execute(viewport, [plan, n, currentState]() {
                            n->tx_->perform();
                        });
                        if (res <= 0)
                            std::cerr << "[WARN] Context failed while: " << n->name_ << std::endl;
                    }
                }
                currentState = BranchState();
            }
        }

        size_t lockCnt = GlobalState::countNodeLocks();
        PLAN_Assert(branchStateStack_.empty());
        PLAN_Assert(lockCnt == 0);

    } catch (std::exception& ex) {
        if (!branchStateStack_.empty() && branchStateStack_.front().isLocked_)
            GlobalState::tryUnlockNode(currentState.branchID_);
        throw;
    } catch (...) {
        if (!branchStateStack_.empty() && branchStateStack_.front().isLocked_)
            GlobalState::tryUnlockNode(currentState.branchID_);
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

Ptr<Plan> Plan::endBranch() {
    auto current = branchStack_.front();
    branchStack_.pop_front();
    string id = "[end]" + current.first;
    emit_access(id, R(*this));
    std::function functor = [](){ return true; };
    add_transaction(current.second, runtime_->plainCtx(), id, functor);
    return self<Plan>();
}

Ptr<Plan> Plan::elseBranch() {
    auto current = branchStack_.front();
    string id = "[else]" + current.first;
    emit_access(id, R(*this));
    std::function functor = [](){ return true; };
    add_transaction(current.second, runtime_->plainCtx(), id, functor);
    return self<Plan>();
}

} // namespace plan

