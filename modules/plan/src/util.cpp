// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
#include <opencv2/core.hpp>
#include "../include/opencv2/plan/util.hpp"
#include <csignal>
#include <unistd.h>
#include <chrono>
#include <mutex>
#include <functional>
#include <iostream>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

using std::cerr;
using std::endl;

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

#ifdef _WIN32
void setThreadName(const char* threadName)
{
    // Windows thread naming via SetThreadDescription (Win10 1607+)
    // Fallback: no-op
    CV_UNUSED(threadName);
}
#else
void setThreadName(const char* threadName)
{
    pthread_setname_np(pthread_self(), threadName);
}
#endif

static std::mutex finish_mtx;
static bool finish_requested = false;
static bool signal_handlers_installed = false;

static void request_finish_signal(int ignore) {
    std::lock_guard guard(finish_mtx);
    CV_UNUSED(ignore);
    finish_requested = true;
}

static void install_signal_handlers() {
    signal(SIGINT, request_finish_signal);
    signal(SIGTERM, request_finish_signal);
}

bool keep_running() {
    std::lock_guard guard(finish_mtx);
    if (!signal_handlers_installed) {
        install_signal_handlers();
    }
    return !finish_requested;
}

void request_finish() {
    request_finish_signal(0);
}

} // namespace plan
} // namespace cv

