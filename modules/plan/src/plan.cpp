// This file is part of OpenCV project. It is subject to the license terms
// in the LICENSE file found in the top-level directory of this distribution.
#include "opencv2/plan/plan.hpp"

#include <chrono>
#include <thread>

namespace cv {
namespace plan {

CV_EXPORTS std::mutex Runtime::instance_mtx_;
CV_EXPORTS thread_local cv::Ptr<Runtime> Runtime::instance_;
CV_EXPORTS thread_local ThreadSafeAnyMap<Runtime::Keys::Enum> Runtime::properties_;

Runtime::Runtime(const cv::Size& size, const string& title) : size_(size), title_(title) {
    fbContext_ = cv::makePtr<detail::FrameBufferContext>(size_);
    sourceContext_ = cv::makePtr<detail::SourceContext>();
    sinkContext_ = cv::makePtr<detail::SinkContext>();
    plainContext_ = cv::makePtr<detail::PlainContext>();
    sourceContext_->setRuntime(this);
    sinkContext_->setRuntime(this);
}

Runtime::Runtime(const Runtime& other, const string& title) : size_(other.size_), title_(title) {
    fbContext_ = cv::makePtr<detail::FrameBufferContext>(size_);
    sourceContext_ = cv::makePtr<detail::SourceContext>();
    sinkContext_ = cv::makePtr<detail::SinkContext>();
    plainContext_ = cv::makePtr<detail::PlainContext>();
    sourceContext_->setRuntime(this);
    sinkContext_->setRuntime(this);
}

Runtime::~Runtime() {}

cv::Ptr<Runtime> Runtime::init(const cv::Size& size, const string& title) {
    GlobalState::init_keys();
    LocalState::init_keys();
    {
        std::lock_guard guard(instance_mtx_);
        if (instance_ == nullptr)
            instance_ = new Runtime(size, title);
    }
    Runtime::init_keys();
    return instance_;
}

cv::Ptr<Runtime> Runtime::init(const Runtime& other, const string& title) {
    GlobalState::init_keys();
    LocalState::init_keys();
    {
        std::lock_guard guard(instance_mtx_);
        if (instance_ == nullptr)
            instance_ = new Runtime(other, title);
    }
    Runtime::init_keys();
    return instance_;
}

std::string Runtime::title() const { return title_; }
const cv::Size& Runtime::size() { return size_; }

cv::Ptr<detail::FrameBufferContext> Runtime::fbCtx() { return fbContext_; }
cv::Ptr<detail::SourceContext> Runtime::sourceCtx() { return sourceContext_; }
cv::Ptr<detail::SinkContext> Runtime::sinkCtx() { return sinkContext_; }
cv::Ptr<detail::PlainContext> Runtime::plainCtx() { return plainContext_; }

cv::Ptr<detail::PlanContext> Runtime::context(const string& name, int32_t idx) {
    std::lock_guard lock(ctxMtx_);
    auto key = std::make_pair(name, idx);
    auto it = contexts_.find(key);
    if (it != contexts_.end())
        return it->second;
    auto ctx = cv::makePtr<detail::PlainContext>();
    contexts_[key] = ctx;
    return ctx;
}

void Runtime::registerContext(const string& name, cv::Ptr<detail::PlanContext> ctx, int32_t idx) {
    std::lock_guard lock(ctxMtx_);
    contexts_[std::make_pair(name, idx)] = ctx;
}

void Runtime::setSource(cv::Ptr<Source> src) { source_ = src; }
cv::Ptr<Source> Runtime::getSource() { return source_; }
bool Runtime::hasSource() const { return source_ != nullptr; }
void Runtime::setSink(cv::Ptr<Sink> sink) { sink_ = sink; }
cv::Ptr<Sink> Runtime::getSink() { return sink_; }
bool Runtime::hasSink() const { return sink_ != nullptr; }

void Runtime::setDisplayCallback(std::function<bool()> cb) { displayCallback_ = cb; }

bool Runtime::display() {
    if (displayCallback_)
        return displayCallback_();
    return keep_running();
}

void Runtime::run(cv::Ptr<Runtime> runtime, std::function<void()> runGraph) {
    static Resequence reseq(1);
    try {
        if (GlobalState::isMain()) {
            // Main thread keeps the process alive; workers drive the frames.
            while (keep_running()) {
                event::poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else {
            while (keep_running()) {
                bool result = true;
                TimeTracker::getInstance()->execute("worker", [&result, runtime, runGraph]() {
                    event::poll();
                    GlobalState::apply<size_t>(GlobalState::Keys::RUN_CNT, [](size_t& s) { ++s; return s; });
                    size_t seq = GlobalState::apply<size_t>(GlobalState::Keys::FRAME_CNT, [](size_t& s) { ++s; return s; });
                    runGraph();
                    reseq.waitFor(seq, [&result, runtime](uint64_t s) {
                        CV_UNUSED(s);
                        result = runtime->display();
                    });
                });
                if (!result)
                    break;
            }
        }
    } catch (std::runtime_error& ex) {
        CV_LOG_WARNING(nullptr, "Pipeline terminated: " << ex.what());
    } catch (std::exception& ex) {
        CV_LOG_WARNING(nullptr, "Pipeline terminated: " << ex.what());
    } catch (...) {
        CV_LOG_WARNING(nullptr, "Pipeline terminated with unknown error.");
    }
    request_finish();
    reseq.finish();
}

} // namespace plan
} // namespace cv

