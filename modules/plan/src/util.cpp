// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
#include "opencv2/plan/util.hpp"
#include <csignal>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace plan {

PLAN_EXPORTS ThreadSafeAnyMap<GlobalState::Keys::Enum> GlobalState::map_;
PLAN_EXPORTS std::mutex GlobalState::threadIDMtx_;
PLAN_EXPORTS const std::thread::id GlobalState::defaultThreadID_;
PLAN_EXPORTS std::thread::id GlobalState::mainThreadID_;
PLAN_EXPORTS bool GlobalState::isFirstRun_ = true;
PLAN_EXPORTS std::set<string> GlobalState::once_;
PLAN_EXPORTS std::mutex GlobalState::nodeLockMtx_;
PLAN_EXPORTS std::map<string,
    std::pair<std::thread::id, Ptr<std::mutex>>> GlobalState::nodeLockMap_;
PLAN_EXPORTS SharedVariables GlobalState::sharedVars_;
PLAN_EXPORTS thread_local ThreadSafeAnyMap<LocalState::Keys::Enum> LocalState::map_;

void GlobalState::init_keys() {
    if (map_.empty()) {
        create<false, uint64_t>(Keys::FRAME_CNT, 0);
        create<false, uint64_t>(Keys::CAPTURE_CNT, 0);
        create<false, uint64_t>(Keys::FPS_CNT, 0);
        create<false, size_t>(Keys::RUN_CNT, 0);
        create<false, uint64_t>(Keys::START_TIME, get_epoch_nanos());
        create<false, double>(Keys::FPS, 0);
        create<false, size_t>(Keys::WORKERS_READY, 0);
        create<false, size_t>(Keys::WORKERS_STARTED, 0);
        create<false, bool>(Keys::LOCKING, false);
        create<false, bool>(Keys::DISPLAY_READY, false);
        create<false, uint64_t>(Keys::LOCK_CONTENTION_CNT, 0);
        create<false, double>(Keys::LOCK_CONTENTION_RATE, 0.0);
        create<false, uint64_t>(Keys::LCR_CNT, 0);
        create<false, bool>(Keys::SHOW_GUI, true);
        create<false, bool>(Keys::TIME_TRACKER, true);
    }
}

SharedVariables& GlobalState::shared_vars() { return sharedVars_; }

Ptr<std::mutex> GlobalState::getNodeLockInternal(const string& name, const bool owned) {
    auto it = nodeLockMap_.find(name);
    if (owned) {
        if (it != nodeLockMap_.end()) {
            if (it->second.first == std::this_thread::get_id())
                return it->second.second;
        } else {
            auto mtxPtr = makePtr<std::mutex>();
            nodeLockMap_[name] = {std::this_thread::get_id(), mtxPtr};
            return mtxPtr;
        }
    } else {
        if (it != nodeLockMap_.end()) {
            if (it->second.first != std::this_thread::get_id())
                return it->second.second;
        }
    }
    return nullptr;
}

bool GlobalState::invalidateNodeLockInternal(const string& name) {
    auto it = nodeLockMap_.find(name);
    if (it != nodeLockMap_.end()) {
        it->second.second = nullptr;
        return true;
    }
    return false;
}

void GlobalState::setMainID(const std::thread::id& id) {
    std::lock_guard<std::mutex> lock(threadIDMtx_);
    mainThreadID_ = id;
}

bool GlobalState::isMain() {
    std::lock_guard<std::mutex> lock(threadIDMtx_);
    return (mainThreadID_ == defaultThreadID_ || mainThreadID_ == std::this_thread::get_id());
}

bool GlobalState::isFirstRun() {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    bool f = isFirstRun_;
    isFirstRun_ = false;
    return f;
}

Ptr<std::mutex> GlobalState::tryGetNodeLock(const string& name) {
    std::lock_guard guard(nodeLockMtx_);
    return getNodeLockInternal(name, false);
}

bool GlobalState::lockNode(const string& name) {
    std::lock_guard guard(nodeLockMtx_);
    auto lock = getNodeLockInternal(name);
    if (lock) { lock->lock(); return true; }
    return false;
}

bool GlobalState::tryUnlockNode(const string& name) {
    std::lock_guard guard(nodeLockMtx_);
    auto lock = getNodeLockInternal(name);
    if (lock) {
        lock->try_lock();
        lock->unlock();
        PLAN_Assert(invalidateNodeLockInternal(name));
        return true;
    }
    return false;
}

size_t GlobalState::countNodeLocks() {
    std::lock_guard guard(nodeLockMtx_);
    size_t cnt = 0;
    for (auto& entry : nodeLockMap_) {
        if (entry.second.first == std::this_thread::get_id() && entry.second.second)
            ++cnt;
    }
    return cnt;
}

bool GlobalState::once(string name) {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    string stem = name.substr(0, name.find_last_of("-"));
    if (once_.find(stem) != once_.end())
        return false;
    once_.insert(stem);
    return true;
}

void setThreadName(const char* threadName) {
#ifdef _WIN32
    PLAN_UNUSED(threadName);
#else
    pthread_setname_np(pthread_self(), threadName);
#endif
}

static std::mutex finish_mtx;
static bool finish_requested = false;
static bool signal_handlers_installed = false;

static void request_finish_handler(int ignore) {
    std::lock_guard guard(finish_mtx);
    PLAN_UNUSED(ignore);
    finish_requested = true;
}

static void install_signal_handlers() {
    signal(SIGINT, request_finish_handler);
    signal(SIGTERM, request_finish_handler);
}

bool keep_running() {
    std::lock_guard guard(finish_mtx);
    if (!signal_handlers_installed)
        install_signal_handlers();
    return !finish_requested;
}

void request_finish() { request_finish_handler(0); }

} // namespace plan

