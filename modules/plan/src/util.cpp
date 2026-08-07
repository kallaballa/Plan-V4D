// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
#include "opencv2/plan/util.hpp"

#include <opencv2/imgproc.hpp>

#include <csignal>
#include <chrono>
#include <mutex>
#include <functional>
#include <iostream>
#include <cmath>

#if defined(_WIN32)
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

#if defined(_WIN32)
void setThreadName(const char* threadName) {
    CV_UNUSED(threadName);
    //best effort: thread naming on Windows is not implemented
}
#elif defined(__APPLE__)
void setThreadName(const char* threadName) {
    pthread_setname_np(threadName);
}
#else
void setThreadName(const char* threadName) {
    pthread_setname_np(pthread_self(), threadName);
}
#endif

size_t cnz(const cv::UMat& m) {
    cv::UMat grey;
    if(m.channels() == 1) {
        grey = m;
    } else if(m.channels() == 3) {
        cvtColor(m, grey, cv::COLOR_BGR2GRAY);
    } else if(m.channels() == 4) {
        cvtColor(m, grey, cv::COLOR_BGRA2GRAY);
    } else {
        CV_Assert(false);
    }
    return cv::countNonZero(grey);
}

size_t cnz(const cv::Mat& m) {
    cv::Mat grey;
    if(m.channels() == 1) {
        grey = m;
    } else if(m.channels() == 3) {
        cvtColor(m, grey, cv::COLOR_BGR2GRAY);
    } else if(m.channels() == 4) {
        cvtColor(m, grey, cv::COLOR_BGRA2GRAY);
    } else {
        CV_Assert(false);
    }
    return cv::countNonZero(grey);
}

CV_EXPORTS void copy_cross(const cv::UMat& src, cv::UMat& dst) {
    if(dst.empty())
        dst.create(src.size(), src.type());
    Mat m = dst.getMat(cv::ACCESS_WRITE);
    src.copyTo(m);
}

static std::mutex finish_mtx;

/*!
 * Internal variable that signals that finishing all operation is requested
 */
static bool finish_requested = false;

/*!
 * Internal variable that tracks if signal handlers have already been installed
 */
static bool signal_handlers_installed = false;

/*!
 * Signal handler callback that signals the application to terminate.
 * @param ignore We ignore the signal number
 */
static void request_finish(int ignore) {
    std::lock_guard guard(finish_mtx);
    CV_UNUSED(ignore);
    finish_requested = true;
}

/*!
 * Installs #request_finish() as signal handler for SIGINT and SIGTERM
 */
static void install_signal_handlers() {
    signal(SIGINT, request_finish);
    signal(SIGTERM, request_finish);
}

bool keep_running() {
    std::lock_guard guard(finish_mtx);
    if (!signal_handlers_installed) {
        install_signal_handlers();
    }
    return !finish_requested;
}

void request_finish() {
    request_finish(0);
}

void reset_finish() {
    std::lock_guard guard(finish_mtx);
    finish_requested = false;
}

float aspect_preserving_scale(const cv::Size& scaled, const cv::Size& unscaled) {
    double scale;
    double scaleX =  double(scaled.width) / unscaled.width;
    double scaleY = double(scaled.height) / unscaled.height;
    if(scaleX < 1.0 && scaleY >= 1.0) {
        scale =  double(unscaled.height) / scaled.height;
    } else if(scaleX >= 1.0 && scaleY < 1.0) {
        scale =  double(unscaled.width) / scaled.width;
    } else
        scale = std::min(scaleX, scaleY);
    return scale;
}

void resize_preserving_aspect_ratio(const cv::UMat& src, cv::UMat& output, const cv::Size& dstSize, const cv::Scalar& bgcolor) {
    cv::UMat tmp;
    double f = aspect_preserving_scale(dstSize, src.size());
    cv::resize(src, tmp, cv::Size(), f, f);
    int top = std::abs((dstSize.height - tmp.rows) / 2);
    int down = std::abs((dstSize.height - tmp.rows + 1) / 2);
    int left = std::abs((dstSize.width - tmp.cols) / 2);
    int right = std::abs((dstSize.width - tmp.cols + 1) / 2);
    cv::copyMakeBorder(tmp, output, top, down, left, right, cv::BORDER_CONSTANT, bgcolor);
}

} /* namespace plan */
} /* namespace cv */
