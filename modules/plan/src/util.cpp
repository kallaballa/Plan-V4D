// This file is part of OpenCV project. It is subject to the license terms
// in the LICENSE file found in the top-level directory of this distribution.
#include "opencv2/plan/util.hpp"

#include <csignal>
#include <chrono>
#include <mutex>
#include <iostream>

#if defined(__linux__) || defined(__APPLE__)
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

void setThreadName(const char* threadName) {
#if defined(__linux__) || defined(__APPLE__)
    pthread_setname_np(pthread_self(), threadName);
#else
    CV_UNUSED(threadName);
#endif
}

static std::mutex finish_mtx;
static bool finish_requested = false;
static bool signal_handlers_installed = false;

static void request_finish_internal(int ignore) {
    std::lock_guard guard(finish_mtx);
    CV_UNUSED(ignore);
    finish_requested = true;
}

static void install_signal_handlers() {
    signal(SIGINT, request_finish_internal);
    signal(SIGTERM, request_finish_internal);
}

bool keep_running() {
    std::lock_guard guard(finish_mtx);
    if (!signal_handlers_installed) {
        install_signal_handlers();
    }
    return !finish_requested;
}

void request_finish() {
    request_finish_internal(0);
}

} // namespace plan
} // namespace cv

