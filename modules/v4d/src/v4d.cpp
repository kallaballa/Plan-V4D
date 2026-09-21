// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include <sstream>
#include <algorithm>
#include <opencv2/core.hpp>
#include <vector>
#include <semaphore>

#include "../include/opencv2/v4d/v4d.hpp"
#include "../include/opencv2/v4d/detail/gl.hpp"
#include "../../third/imgui/backends/imgui_impl_glfw.h"

namespace gwe {
namespace detail {
CV_EXPORTS std::vector<EventQueue*> Holder::queue_vector;
}
}


namespace cv {
namespace v4d {

CV_EXPORTS std::mutex V4D::instance_mtx_;
CV_EXPORTS thread_local cv::Ptr<V4D> V4D::instance_;
CV_EXPORTS thread_local ThreadSafeAnyMap<V4D::Keys::Enum> V4D::properties_;

cv::Ptr<V4D> V4D::init(const cv::Rect& viewport, const string& title, AllocateFlags::Enum allocFlags, ConfigFlags::Enum confFlags, DebugFlags::Enum debFlags, int samples) {
	GlobalState::init_keys();
	LocalState::init_keys();
	{
        std::lock_guard guard(instance_mtx_);
        if(instance_ == nullptr)
            instance_ = new V4D(viewport, cv::Size(), title, allocFlags, confFlags, debFlags, samples);
    }
    V4D::init_keys();
    PlanRuntime::current() = instance_;
	return instance_;
}

cv::Ptr<V4D> V4D::init(const cv::Rect& viewport, const cv::Size& fbSize, const string& title, AllocateFlags::Enum allocFlags, ConfigFlags::Enum confFlags, DebugFlags::Enum debFlags, int samples) {
	CV_UNUSED(fbSize);
	GlobalState::init_keys();
	LocalState::init_keys();
	{
        std::lock_guard guard(instance_mtx_);
        if(instance_ == nullptr)
            instance_ = new V4D(viewport, cv::Size(), title, allocFlags, confFlags, debFlags, samples);
    }

    V4D::init_keys();
    PlanRuntime::current() = instance_;
	return instance_;
}

cv::Ptr<V4D> V4D::init(const V4D& other, const string& title) {
	LocalState::init_keys();
	std::lock_guard guard(instance_mtx_);
	if(instance_ == nullptr)
	    instance_ = new V4D(other, title);

	V4D::init_keys();
	PlanRuntime::current() = instance_;
	return instance_;
}

V4D::V4D(const cv::Rect& viewport, cv::Size fbsize, const string& title, AllocateFlags::Enum allocFlags, ConfigFlags::Enum confFlags, DebugFlags::Enum debFlags, int samples) :
        allocateFlags_(allocFlags), configFlags_(confFlags), debugFlags_(debFlags), samples_(samples) {

    int fbFlags = (configFlags() &  ConfigFlags::DISPLAY_MODE ? FBConfigFlags::VSYNC : 0)
    		| (debugFlags() &  DebugFlags::DEBUG_GL_CONTEXT ? FBConfigFlags::DEBUG_GL_CONTEXT : 0)
			| (debugFlags() &  DebugFlags::ONSCREEN_CONTEXTS ? FBConfigFlags::ONSCREEN_CHILD_CONTEXTS : 0)
			| (configFlags() &  ConfigFlags::OFFSCREEN ? FBConfigFlags::OFFSCREEN : 0)
			| (configFlags() &  ConfigFlags::RESIZEABLE ? FBConfigFlags::RESIZEABLE : 0);

    mainFbContext_ = detail::FrameBufferContext::make(fbsize.empty() ? viewport.size() : fbsize, title, 3,
                2, samples, nullptr, nullptr, true, fbFlags);
    CLExecScope_t scope(mainFbContext_->getCLExecContext());
    sourceContext_ = new detail::SourceContext(mainFbContext_);
    sinkContext_ = new detail::SinkContext(mainFbContext_);

    if(allocateFlags() & AllocateFlags::IMGUI)
        imguiContext_ = new detail::ImGuiContextImpl(mainFbContext_);

    if(allocateFlags() & AllocateFlags::NANOVG)
       	nvgContext_ = new detail::NanoVGContext(mainFbContext_);

}

V4D::V4D(const V4D& other, const string& title) :
		allocateFlags_(other.allocateFlags_), configFlags_(other.configFlags_), debugFlags_(other.debugFlags_), samples_(other.samples_) {
	int fbFlags = (configFlags() &  ConfigFlags::DISPLAY_MODE ? FBConfigFlags::DISPLAY_MODE : 0)
    		| (debugFlags() &  DebugFlags::DEBUG_GL_CONTEXT ? FBConfigFlags::DEBUG_GL_CONTEXT : 0)
			| (debugFlags() &  DebugFlags::ONSCREEN_CONTEXTS ? FBConfigFlags::ONSCREEN_CHILD_CONTEXTS : FBConfigFlags::OFFSCREEN);

    mainFbContext_ = detail::FrameBufferContext::make(other.mainFbContext_->size(), title, 3,
                2, other.samples_, other.mainFbContext_->glfwWindow_, other.mainFbContext_, true, fbFlags);
    CLExecScope_t scope(mainFbContext_->getCLExecContext());
    if(allocateFlags() & AllocateFlags::NANOVG)
    	nvgContext_ = new detail::NanoVGContext(mainFbContext_);
    if(allocateFlags() & AllocateFlags::BGFX)
        bgfxContext_ = new detail::BgfxContext(mainFbContext_);
    sourceContext_ = new detail::SourceContext(mainFbContext_);
    sinkContext_ = new detail::SinkContext(mainFbContext_);
    plainContext_ = new cv::plan::detail::PlainContext();
}

V4D::~V4D() {

}

std::string V4D::title() const {
    return mainFbContext_->title_;
}

cv::Ptr<cv::plan::detail::PlanContext> V4D::fbCtx() {
    assert(mainFbContext_ != nullptr);
    return mainFbContext_;
}

cv::Ptr<cv::plan::detail::PlanContext> V4D::sourceCtx() {
    assert(sourceContext_ != nullptr);
    return sourceContext_;
}

cv::Ptr<cv::plan::detail::PlanContext> V4D::sinkCtx() {
    assert(sinkContext_ != nullptr);
    return sinkContext_;
}

cv::Ptr<cv::plan::detail::PlanContext> V4D::nvgCtx() {
    assert(nvgContext_ != nullptr);
    return nvgContext_;
}

cv::Ptr<cv::plan::detail::PlanContext> V4D::bgfxCtx() {
    assert(bgfxContext_ != nullptr);
    return bgfxContext_;
}

cv::Ptr<cv::plan::detail::PlainContext> V4D::plainCtx() {
    assert(plainContext_ != nullptr);
    return plainContext_;
}

cv::Ptr<cv::plan::detail::PlanContext> V4D::imguiCtx() {
    assert(imguiContext_ != nullptr);
    return imguiContext_;
}

cv::Ptr<cv::plan::detail::PlanContext> V4D::glCtx(int32_t idx) {
    auto it = glContexts_.find(idx);
    if(it != glContexts_.end())
        return (*it).second;
    else {
        cv::Ptr<GLContext> ctx = new GLContext(idx, mainFbContext_);
        glContexts_.insert({idx, ctx});
        return ctx;
    }
}

cv::Ptr<cv::plan::detail::PlanContext> V4D::extCtx(int32_t idx) {
    auto it = extContexts_.find(idx);
    if(it != extContexts_.end())
        return (*it).second;
    else {
        cv::Ptr<ExtContext> ctx = new ExtContext(idx, mainFbContext_);
        extContexts_.insert({idx, ctx});
        return ctx;
    }
}

bool V4D::hasFbCtx() {
    return mainFbContext_ != nullptr;
}

bool V4D::hasSourceCtx() {
    return sourceContext_ != nullptr;
}

bool V4D::hasSinkCtx() {
    return sinkContext_ != nullptr;
}

bool V4D::hasNvgCtx() {
    return nvgContext_ != nullptr;
}

bool V4D::hasBgfxCtx() {
    return bgfxContext_ != nullptr;
}

bool V4D::hasPlainCtx() {
    return plainContext_ != nullptr;
}

bool V4D::hasImguiCtx() {
    return imguiContext_ != nullptr;
}

bool V4D::hasGlCtx(uint32_t idx) {
    return glContexts_.find(idx) != glContexts_.end();
}

bool V4D::hasExtCtx(uint32_t idx) {
    return extContexts_.find(idx) != extContexts_.end();
}

void V4D::copyTo(cv::UMat& m) {
	mainFbContext_->copyTo(m);
}

void V4D::copyFrom(const cv::UMat& m) {
	mainFbContext_->copyFrom(m);
}

void V4D::setSource(cv::Ptr<Source> src) {
    source_ = src;
}
cv::Ptr<Source> V4D::getSource() {
    return source_;
}

bool V4D::hasSource() const {
    return source_ != nullptr;
}

void V4D::setSink(cv::Ptr<Sink> sink) {
    sink_ = sink;
}

cv::Ptr<Sink> V4D::getSink() {
    return sink_;
}

bool V4D::hasSink() const {
    return sink_ != nullptr;
}

cv::Vec2f V4D::position() {
    return mainFbContext_->position();
}

float V4D::pixelRatioX() {
    return mainFbContext_->pixelRatioX();
}

float V4D::pixelRatioY() {
    return mainFbContext_->pixelRatioY();
}

const cv::Size& V4D::size() {
    return get<cv::Size>(Keys::WINDOW_SIZE);
}

void V4D::setShowFPS(bool s) {
    showFPS_ = s;
}

bool V4D::getShowFPS() {
    return showFPS_;
}

void V4D::setPrintFPS(bool p) {
    printFPS_ = p;
}

bool V4D::getPrintFPS() {
    return printFPS_;
}

void V4D::setShowTracking(bool st) {
    showTracking_ = st;
}

bool V4D::getShowTracking() {
    return showTracking_;
}

void V4D::swapContextBuffers() {
	cv::Rect fbViewport(0, 0, mainFbContext_->size().width, mainFbContext_->size().height);
	int32_t numGl = std::max(off_t(0), off_t(glContexts_.size()) - 1);
    for(int32_t i = -1; i < numGl; ++i) {
    	FrameBufferContext::WindowScope winScope(glContexts_[i]->fbCtx());
        FrameBufferContext::GLScope glScope(glContexts_[i]->fbCtx(), GL_READ_FRAMEBUFFER);
        GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0));
        assert(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        glContexts_[i]->fbCtx()->blitFrameBufferToFrameBuffer(fbViewport, mainFbContext_->size(), false);
        GL_CHECK(glFinish());
        glfwSwapBuffers(glContexts_[i]->fbCtx()->getGLFWWindow());
    }

    if(hasNvgCtx()) {
    	FrameBufferContext::WindowScope winScope(nvgContext_->fbCtx());
		FrameBufferContext::GLScope glScope(nvgContext_->fbCtx(), GL_READ_FRAMEBUFFER);

		GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0));
        assert(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
		nvgContext_->fbCtx()->blitFrameBufferToFrameBuffer(fbViewport, mainFbContext_->size(), false);
		glfwSwapBuffers(nvgContext_->fbCtx()->getGLFWWindow());
    }

    if(hasBgfxCtx()) {
    	FrameBufferContext::WindowScope winScope(bgfxContext_->fbCtx());
		FrameBufferContext::GLScope glScope(bgfxContext_->fbCtx(), GL_READ_FRAMEBUFFER);

        GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0));
        assert(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
		bgfxContext_->fbCtx()->blitFrameBufferToFrameBuffer(fbViewport, mainFbContext_->size(), false);
		glfwSwapBuffers(bgfxContext_->fbCtx()->getGLFWWindow());
    }

}

bool V4D::display() {
    auto startDisplayFuncNanos = get_epoch_nanos();

    if(!GlobalState::isMain()) {
        GlobalState::apply<uint64_t>(GlobalState::Keys::FPS_CNT, [](uint64_t& v){ return v++; });
        GlobalState::apply<uint64_t>(GlobalState::Keys::LCR_CNT, [](uint64_t& v){ return v++; });

		if(debugFlags() & DebugFlags::ONSCREEN_CONTEXTS) {
			swapContextBuffers();
		}
    }
	if (GlobalState::isMain()) {
		bool countLockContention = debugFlags() & DebugFlags::PRINT_LOCK_CONTENTION;
		auto start = GlobalState::get<uint64_t>(GlobalState::Keys::START_TIME);
		auto now = get_epoch_nanos();
		auto diff = now - start;
		double diffSeconds = diff / 1000000000.0;

		if(GlobalState::get<double>(GlobalState::Keys::FPS) > 0 && diffSeconds > 1.0) {
		    GlobalState::apply<uint64_t>(GlobalState::Keys::START_TIME, [diff](uint64_t& v) { return (v += (diff / 2.0)); } );
		    GlobalState::apply<uint64_t>(GlobalState::Keys::FPS_CNT, [diff](uint64_t& v) { return (v *= 0.5); } );

            if(countLockContention) {
	            GlobalState::apply<uint64_t>(GlobalState::Keys::LCR_CNT, [diff](uint64_t& v) { return (v *= 0.5); } );
			}
		} else {
			double fps = GlobalState::get<double>(GlobalState::Keys::FPS);
            uint64_t fpsCnt = GlobalState::get<uint64_t>(GlobalState::Keys::FPS_CNT);

			GlobalState::set(GlobalState::Keys::FPS, (fps * 3.0 + (fpsCnt / diffSeconds)) / 4.0);
			if(countLockContention) {
				double rate = GlobalState::get<double>(GlobalState::Keys::LOCK_CONTENTION_RATE);
	            uint64_t lcrCnt = GlobalState::get<uint64_t>(GlobalState::Keys::LCR_CNT);
				GlobalState::set(GlobalState::Keys::LOCK_CONTENTION_RATE, (rate * 3.0 + (lcrCnt / diffSeconds)) / 4.0);
			}
		}

		if(countLockContention) {
			std::cerr << "\rLPS:" << GlobalState::get<double>(GlobalState::Keys::LOCK_CONTENTION_RATE) << std::endl;
		}

		if(getPrintFPS()) {
			std::cerr << "\rFPS:" << GlobalState::get<double>(GlobalState::Keys::FPS) << std::endl;
		}

        cv::Rect vp = get<cv::Rect>(Keys::VIEWPORT);
		{
			FrameBufferContext::WindowScope winScope(mainFbContext_);
			FrameBufferContext::GLScope glScope(mainFbContext_, GL_READ_FRAMEBUFFER);
			GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0));
			assert(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
			mainFbContext_->blitFrameBufferToFrameBuffer(vp, mainFbContext_->size(), false, false);
		}
		{
			if((allocateFlags() & AllocateFlags::IMGUI) && GlobalState::get<bool>(GlobalState::Keys::SHOW_GUI)) {
				FrameBufferContext::WindowScope winScope(mainFbContext_);
				FrameBufferContext::GLScope glScope(mainFbContext_, GL_DRAW_FRAMEBUFFER, 0);

#if !defined(OPENCV_V4D_USE_ES3)
				GL_CHECK(glDrawBuffer(GL_BACK));
#endif
				if(hasImguiCtx())
					imguiContext_->execute(vp, [](){});
			}
		}

		TimeTracker::getInstance()->setEnabled(GlobalState::get<bool>(GlobalState::Keys::TIME_TRACKER));
		TimeTracker::getInstance()->newCount();
		GL_CHECK(glFinish());
		glfwSwapBuffers(mainFbContext_->getGLFWWindow());


		if(!(configFlags() &  ConfigFlags::DISPLAY_MODE)) {
	        auto endDisplayFuncNanos = get_epoch_nanos();
	        auto displayDuration = endDisplayFuncNanos - startDisplayFuncNanos;
	        int64_t sleepNanos = std::round((1000000000.0/60.0) - displayDuration);
	        if(sleepNanos > 0) {
	            std::this_thread::sleep_for(std::chrono::nanoseconds(sleepNanos));
		    }
		}
		GlobalState::set(GlobalState::Keys::DISPLAY_READY, true);
		GL_CHECK(glClearColor(0,0,0,1));
		GL_CHECK(glClear(GL_COLOR_BUFFER_BIT));
		bool keepOpen = !glfwWindowShouldClose(getGLFWWindow());
		if(!keepOpen) {
			cv::v4d::request_finish();
		}
		return keepOpen;
	} else {
		if(GlobalState::apply<bool>(GlobalState::Keys::DISPLAY_READY, [](bool& v){
			if(!v)
				return v;
			else {
				bool ret = v;
				v = !v;
				return ret;
			}
		})) {
			mainFbContext_->copyToRootWindow();
		}
		if(debugFlags() & DebugFlags::ONSCREEN_CONTEXTS) {
			FrameBufferContext::WindowScope winScope(mainFbContext_);
			FrameBufferContext::GLScope glScope(mainFbContext_, GL_READ_FRAMEBUFFER);
			cv::Rect initial = get<cv::Rect>(Keys::SIZE);
			initial.y = (mainFbContext_->size().height - initial.height) + initial.y;
	        GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0));
	        assert(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

			mainFbContext_->blitFrameBufferToFrameBuffer(initial, mainFbContext_->size(), false, false);
			glfwSwapBuffers(mainFbContext_->getGLFWWindow());
		}
		GL_CHECK(glFinish());
	}

    return true;
}

GLFWwindow* V4D::getGLFWWindow() const {
    return mainFbContext_->getGLFWWindow();
}

void V4D::printSystemInfo() {
	std::cerr << "OpenGL: " << get_gl_info() << std::endl;
#ifdef HAVE_OPENCL
	if(cv::ocl::useOpenCL())
		std::cerr << "OpenCL Platforms: " << get_cl_info() << endl;
#endif
}

AllocateFlags::Enum V4D::allocateFlags() {
	return allocateFlags_;
}

ConfigFlags::Enum V4D::configFlags() {
	return configFlags_;
}

DebugFlags::Enum V4D::debugFlags() {
	return debugFlags_;
}

}
}
