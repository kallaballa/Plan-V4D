#include "opencv2/plan/util.hpp"
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace cv {
namespace plan {

CV_EXPORTS ThreadSafeAnyMap<GlobalState::Keys::Enum> GlobalState::map_;
CV_EXPORTS std::mutex GlobalState::threadIDMtx_;
CV_EXPORTS const std::thread::id GlobalState::defaultThreadID_;
CV_EXPORTS std::thread::id GlobalState::mainThreadID_;
CV_EXPORTS bool GlobalState::isFirstRun_ = true;
CV_EXPORTS std::set<string> GlobalState::once_;
CV_EXPORTS std::mutex GlobalState::nodeLockMtx_;
CV_EXPORTS std::map<string, std::pair<std::thread::id, cv::Ptr<std::mutex>>> GlobalState::nodeLockMap_;
CV_EXPORTS SharedVariables GlobalState::sharedVars_;

CV_EXPORTS thread_local ThreadSafeAnyMap<LocalState::Keys::Enum> LocalState::map_;

void GlobalState::init_keys() {
    if(map_.empty()) {
        create<false, uint64_t>(Keys::FRAME_CNT, 0);
        create<false, size_t>(Keys::RUN_CNT, 0);
        create<false, uint64_t>(Keys::START_TIME, get_epoch_nanos());
        create<false, double>(Keys::FPS, 0);
        create<false, size_t>(Keys::WORKERS_READY, 0);
        create<false, size_t>(Keys::WORKERS_STARTED, 0);
        create<false, bool>(Keys::LOCKING, false);
        create<false, uint64_t>(Keys::LOCK_CONTENTION_CNT, 0);
        create<false, double>(Keys::LOCK_CONTENTION_RATE, 0.0);
        create<false, uint64_t>(Keys::LCR_CNT, 0);
        create<false, bool>(Keys::TIME_TRACKER, true);
    }
}

SharedVariables& GlobalState::shared_vars() {
    return sharedVars_;
}

cv::Ptr<std::mutex> GlobalState::getNodeLockInternal(const string& name, const bool owned) {
    auto it = nodeLockMap_.find(name);
    if(owned) {
        if(it != nodeLockMap_.end()) {
            auto entry = *it;
            if(entry.second.first == std::this_thread::get_id()) {
                return entry.second.second;
            }
        } else {
            auto mtxPtr = cv::makePtr<std::mutex>();
            nodeLockMap_[name] = {std::this_thread::get_id(), mtxPtr};
            return mtxPtr;
        }
    } else {
        if(it != nodeLockMap_.end()) {
            auto entry = *it;
            if(entry.second.first != std::this_thread::get_id()) {
                return entry.second.second;
            }
        }
    }
    return nullptr;
}

bool GlobalState::invalidateNodeLockInternal(const string& name) {
    auto it = nodeLockMap_.find(name);
    if(it != nodeLockMap_.end()) {
        auto& entry = *it;
        entry.second.second = nullptr;
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

cv::Ptr<std::mutex> GlobalState::tryGetNodeLock(const string& name) {
    std::lock_guard guard(nodeLockMtx_);
    return getNodeLockInternal(name, false);
}

bool GlobalState::lockNode(const string& name) {
    std::lock_guard guard(nodeLockMtx_);
    auto lock = getNodeLockInternal(name);
    if(lock) {
        lock->lock();
        return true;
    }
    return false;
}

bool GlobalState::tryUnlockNode(const string& name) {
    std::lock_guard guard(nodeLockMtx_);
    auto lock = getNodeLockInternal(name);
    if(lock) {
        lock->unlock();
        CV_Assert(invalidateNodeLockInternal(name));
        return true;
    }
    return false;
}

size_t GlobalState::countNodeLocks() {
    std::lock_guard guard(nodeLockMtx_);
    size_t cnt = 0;
    for(auto entry : nodeLockMap_) {
        if(entry.second.first == std::this_thread::get_id() && entry.second.second) {
            ++cnt;
        }
    }
    return cnt;
}

bool GlobalState::once(string name) {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    string stem = name.substr(0, name.find_last_of("-"));
    auto it = once_.find(stem);
    if(it != once_.end()) {
        return false;
    } else {
        once_.insert(stem);
        return true;
    }
}

void setThreadName(const char* threadName) {
#ifdef _WIN32
    // Windows thread naming not trivially supported; skip
    CV_UNUSED(threadName);
#else
    pthread_setname_np(pthread_self(), threadName);
#endif
}

} // namespace plan
} // namespace cv

