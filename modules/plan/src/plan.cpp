// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
#include "opencv2/plan/plan.hpp"

#include <opencv2/core/utils/logger.hpp>

#include <chrono>
#include <cmath>
#include <iostream>

namespace cv {
namespace plan {

CV_EXPORTS std::mutex Runtime::instance_mtx_;
CV_EXPORTS thread_local cv::Ptr<Runtime> Runtime::instance_;
CV_EXPORTS thread_local ThreadSafeAnyMap<Runtime::Keys::Enum> Runtime::properties_;

Runtime::Runtime(const cv::Size& size, const string& title, DebugFlags::Enum debFlags) :
    debugFlags_(debFlags), title_(title) {
    CV_UNUSED(size);
    plainContext_ = new detail::PlainContext();
}

Runtime::Runtime(const Runtime& other, const string& title) :
    debugFlags_(other.debugFlags_), title_(title) {
    plainContext_ = new detail::PlainContext();
}

Runtime::~Runtime() {
}

cv::Ptr<Runtime> Runtime::instance() {
    std::lock_guard guard(instance_mtx_);
    if(!instance_)
        CV_Error(cv::Error::StsAssert, "Runtime not initialized. You have to call the static ```init``` function of the runtime first!");
    return instance_;
}

cv::Ptr<Runtime> Runtime::init(const string& title, DebugFlags::Enum debugFlags) {
    return init(cv::Size(640, 480), title, debugFlags);
}

cv::Ptr<Runtime> Runtime::init(const cv::Size& size, const string& title, DebugFlags::Enum debugFlags) {
    GlobalState::init_keys();
    LocalState::init_keys();
    {
        std::lock_guard guard(instance_mtx_);
        if(instance_ == nullptr)
            instance_ = new Runtime(size, title, debugFlags);
    }
    Runtime::init_keys(size);
    return instance_;
}

cv::Ptr<Runtime> Runtime::init(const Runtime& other, const string& title) {
    GlobalState::init_keys();
    LocalState::init_keys();
    cv::Size sz = other.size();
    {
        std::lock_guard guard(instance_mtx_);
        if(instance_ == nullptr)
            instance_ = new Runtime(other, title);
    }
    Runtime::init_keys(sz);
    return instance_;
}

void Runtime::init_keys(const cv::Size& sz) {
    if(properties_.empty()) {
        create<true>(Keys::SIZE, sz);
        create<false>(Keys::VIEWPORT, cv::Rect(0, 0, sz.width, sz.height));
        create<false, string>(Keys::NAMESPACE, "default");
    }
}

std::string Runtime::title() const {
    return title_;
}

const cv::Size& Runtime::size() {
    return get<cv::Size>(Keys::SIZE);
}

DebugFlags::Enum Runtime::debugFlags() {
    return debugFlags_;
}

void Runtime::updateStats(cv::Ptr<Runtime> runtime) {
    auto start = GlobalState::get<uint64_t>(GlobalState::Keys::START_TIME);
    auto now = get_epoch_nanos();
    auto diff = now - start;
    double diffSeconds = diff / 1000000000.0;
    if(GlobalState::get<double>(GlobalState::Keys::FPS) > 0 && diffSeconds > 1.0) {
        GlobalState::apply<uint64_t>(GlobalState::Keys::START_TIME, [diff](uint64_t& v) { return (v += (diff / 2.0)); } );
        GlobalState::apply<uint64_t>(GlobalState::Keys::FPS_CNT, [diff](uint64_t& v) { return (v *= 0.5); } );
        GlobalState::apply<uint64_t>(GlobalState::Keys::LCR_CNT, [diff](uint64_t& v) { return (v *= 0.5); } );
    } else if(diffSeconds > 0) {
        double fps = GlobalState::get<double>(GlobalState::Keys::FPS);
        uint64_t fpsCnt = GlobalState::get<uint64_t>(GlobalState::Keys::FPS_CNT);
        GlobalState::set(GlobalState::Keys::FPS, (fps * 3.0 + (fpsCnt / diffSeconds)) / 4.0);
        double rate = GlobalState::get<double>(GlobalState::Keys::LOCK_CONTENTION_RATE);
        uint64_t lcrCnt = GlobalState::get<uint64_t>(GlobalState::Keys::LCR_CNT);
        GlobalState::set(GlobalState::Keys::LOCK_CONTENTION_RATE, (rate * 3.0 + (lcrCnt / diffSeconds)) / 4.0);
    }
    if(runtime->debugFlags() & DebugFlags::PRINT_LOCK_CONTENTION) {
        std::cerr << "\rLPS:" << GlobalState::get<double>(GlobalState::Keys::LOCK_CONTENTION_RATE) << std::endl;
    }
    TimeTracker::getInstance()->setEnabled(GlobalState::get<bool>(GlobalState::Keys::TIME_TRACKER));
    TimeTracker::getInstance()->newCount();
}

void Runtime::run(cv::Ptr<Runtime> runtime, std::function<void()> runGraph) {
    try {
        while(keep_running()) {
            TimeTracker::getInstance()->execute("worker", [&runtime, runGraph](){
                GlobalState::apply<size_t>(GlobalState::Keys::RUN_CNT, [](size_t& s) { ++s; return s; });
                GlobalState::apply<size_t>(GlobalState::Keys::FRAME_CNT, [](size_t& s) { ++s; return s; });
                GlobalState::apply<uint64_t>(GlobalState::Keys::FPS_CNT, [](uint64_t& v){ return v++; });
                GlobalState::apply<uint64_t>(GlobalState::Keys::LCR_CNT, [](uint64_t& v){ return v++; });
                runGraph();
                Runtime::updateStats(runtime);
            });
        }
    } catch(std::runtime_error& ex) {
        CV_LOG_WARNING(&plan_tag, "Pipeline terminated: " << ex.what());
    } catch(std::exception& ex) {
        CV_LOG_WARNING(&plan_tag, "Pipeline terminated: " << ex.what());
    } catch(...) {
        CV_LOG_WARNING(&plan_tag, "Pipeline terminated with unknown error.");
    }
    request_finish();
}

} /* namespace plan */
} /* namespace cv */
