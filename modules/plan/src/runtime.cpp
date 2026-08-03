// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
#include "opencv2/plan/runtime.hpp"
#include "opencv2/plan/plan.hpp"
#include <iostream>

namespace plan {

PLAN_EXPORTS std::mutex Runtime::instance_mtx_;
PLAN_EXPORTS thread_local Ptr<Runtime> Runtime::instance_;
PLAN_EXPORTS thread_local ThreadSafeAnyMap<Runtime::Keys::Enum> Runtime::properties_;

Ptr<Runtime> Runtime::instance() {
    std::lock_guard guard(instance_mtx_);
    if (!instance_)
        throw std::runtime_error(
            "Runtime not initialized. Call the static init() function first!");
    return instance_;
}

Ptr<Runtime> Runtime::init(const Size& sz, const string& name) {
    PLAN_UNUSED(name);
    GlobalState::init_keys();
    LocalState::init_keys();
    {
        std::lock_guard guard(instance_mtx_);
        if (instance_ == nullptr)
            instance_ = std::shared_ptr<Runtime>(new Runtime(sz, name));
    }
    Runtime::init_keys();
    return instance_;
}

Runtime::Runtime(const Size& sz, const string& name)
    : size_(sz), viewport_(0, 0, sz.width, sz.height) {
    PLAN_UNUSED(name);
    plainContext_ = makePtr<PlainContext>();
}

Runtime::~Runtime() {}

void Runtime::init_keys() {
    create<true>(Keys::SIZE, instance_->size_);
    create<false>(Keys::VIEWPORT, instance_->viewport_);
    create<false>(Keys::CLEAR_COLOR, Scalar(0, 0, 0, 255));
    create<false, string>(Keys::NAMESPACE, "default");
}

void Runtime::registerContext(const std::string& name, Ptr<Context> ctx) {
    std::lock_guard guard(ctxMtx_);
    contexts_[name] = ctx;
}

Ptr<Context> Runtime::getContext(const std::string& name) {
    std::lock_guard guard(ctxMtx_);
    auto it = contexts_.find(name);
    if (it != contexts_.end())
        return it->second;
    return plainContext_;
}

bool Runtime::hasContext(const std::string& name) {
    std::lock_guard guard(ctxMtx_);
    return contexts_.find(name) != contexts_.end();
}

Ptr<PlainContext> Runtime::plainCtx() {
    PLAN_Assert(plainContext_ != nullptr);
    return plainContext_;
}

Rect Runtime::viewport() const { return viewport_; }
void Runtime::setViewport(const Rect& vp) { viewport_ = vp; }
Size Runtime::size() const { return size_; }
void Runtime::stop() { running_ = false; }
bool Runtime::isRunning() const { return running_; }

void Runtime::run(Ptr<Runtime> runtime, std::function<void()> runGraph) {
    try {
        while (keep_running() && runtime->isRunning()) {
            GlobalState::apply<uint64_t>(GlobalState::Keys::FRAME_CNT,
                [](uint64_t& v) { ++v; return v; });
            GlobalState::apply<size_t>(GlobalState::Keys::RUN_CNT,
                [](size_t& s) { ++s; return s; });
            runGraph();
        }
    } catch (std::exception& ex) {
        std::cerr << "[WARN] Pipeline terminated: " << ex.what() << std::endl;
    } catch (...) {
        std::cerr << "[WARN] Pipeline terminated with unknown error." << std::endl;
    }
    request_finish();
}

} // namespace plan

