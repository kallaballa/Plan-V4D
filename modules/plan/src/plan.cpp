// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include "../include/opencv2/plan/detail/transaction.hpp"
#include "../include/opencv2/plan/util.hpp"

#include <tuple>
#include <functional>
#include <utility>
#include <type_traits>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace cv {
namespace plan {

Transaction::Transaction() : btype_(BranchType::NONE) {
}

bool Transaction::isBranch() {
	return btype_ != BranchType::NONE;
}

void Transaction::setBranchType(BranchType::Enum btype) {
	btype_ = btype;
}

BranchType::Enum Transaction::getBranchType() {
	return btype_;
}

void Transaction::setContextCallback(std::function<cv::Ptr<cv::plan::detail::PlanContext>()> cb) {
	ctxCallback_ = cb;
}

std::function<cv::Ptr<cv::plan::detail::PlanContext>()> Transaction::getContextCallback() {
	return ctxCallback_;
}

// GlobalState static member definitions
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

CV_EXPORTS size_t cnz(const cv::UMat& m) {
    cv::UMat grey;
    if(m.channels() == 1) {
        grey = m;
    } else if(m.channels() == 3) {
        cvtColor(m, grey, cv::COLOR_BGR2GRAY);
    } else if(m.channels() == 4) {
        cvtColor(m, grey, cv::COLOR_BGRA2GRAY);
    } else {
        assert(false);
    }
    return cv::countNonZero(grey);
}

CV_EXPORTS size_t cnz(const cv::Mat& m) {
    cv::Mat grey;
    if(m.channels() == 1) {
        grey = m;
    } else if(m.channels() == 3) {
        cvtColor(m, grey, cv::COLOR_BGR2GRAY);
    } else if(m.channels() == 4) {
        cvtColor(m, grey, cv::COLOR_BGRA2GRAY);
    } else {
        assert(false);
    }
    return cv::countNonZero(grey);
}

void setThreadName(const char* threadName) {
#ifdef __linux__
   pthread_setname_np(pthread_self(), threadName);
#endif
}

}
}
