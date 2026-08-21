// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#ifndef SRC_OPENCV_V4D_V4D_HPP_
#define SRC_OPENCV_V4D_V4D_HPP_

#include <opencv2/plan/plan.hpp>

namespace cv {
namespace v4d {
using namespace cv::plan;
}
}

#include "source.hpp"
#include "sink.hpp"
#include "util.hpp"
#include "nvg.hpp"
#include "detail/framebuffercontext.hpp"
#include "detail/nanovgcontext.hpp"
#include "detail/imguicontext.hpp"
#include "detail/timetracker.hpp"
#include "detail/glcontext.hpp"
#include "detail/extcontext.hpp"
#include "detail/sourcecontext.hpp"
#include "detail/sinkcontext.hpp"
#include "detail/bgfxcontext.hpp"
#include "detail/resequence.hpp"

#define EVENT_API_EXPORT CV_EXPORTS
#include "events.hpp"

#include <shared_mutex>
#include <future>
#include <set>
#include <map>
#include <string>
#include <memory>
#include <vector>
#include <barrier>
#include <type_traits>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/core/utility.hpp>

using namespace std::chrono_literals;
using namespace cv::utils::logging;

namespace cv {
namespace v4d {

const LogTag cf_tag("Flow", LogLevel::LOG_LEVEL_INFO);
const LogTag v4d_tag("V4D", LogLevel::LOG_LEVEL_INFO);
const LogTag mon_tag("Monitor", LogLevel::LOG_LEVEL_INFO);

namespace event {
	using namespace gwe;
	typedef Mouse_<cv::Point> Mouse;
	typedef Window_<cv::Point> Window;

	inline std::vector<std::shared_ptr<Joystick>> fetch(const Joystick::Type& t){
		return fetch<Joystick>(t);
	}

	inline std::vector<std::shared_ptr<Keyboard>> fetch(const Keyboard::Type& t){
		return fetch<Keyboard>(t);
	}

	inline std::vector<std::shared_ptr<Mouse>> fetch(const Mouse::Type& t){
		return fetch<Mouse>(t);
	}

	inline std::vector<std::shared_ptr<Mouse>> fetch(const Mouse::Type& t, const Mouse::Button& b){
		return fetch<Mouse>(t ,b);
	}

	inline std::vector<std::shared_ptr<Window>> fetch(const Window::Type& t){
		return fetch<Window>(t);
	}

	inline bool consume(const Mouse::Type& t){
		return consume<Mouse>(t);
	}

	inline bool consume(const Mouse::Type& t, const Mouse::Button& b){
		return consume<Mouse>(t ,b);
	}

	inline bool consume(const Window::Type& t){
		return consume<Window>(t);
	}

}

using namespace cv::v4d::detail;

class CV_EXPORTS V4D : public PlanRuntime {
    friend class detail::FrameBufferContext;
    friend class detail::SourceContext;
    friend class detail::SinkContext;
    friend class detail::NanoVGContext;
    friend class detail::ImGuiContextImpl;
    friend class cv::plan::detail::PlainContext;
    friend class detail::GLContext;
    friend class detail::ExtContext;
    friend class detail::BgfxContext;
    friend class Source;
    friend class Sink;
    friend class V4DPlan;
public:
    struct Keys {
    	enum Enum {
    		SIZE,
    		VIEWPORT,
			WINDOW_SIZE,
			FRAMEBUFFER_SIZE,
			CLEAR_COLOR,
			NAMESPACE,
			FULLSCREEN,
			DISABLE_INPUT_EVENTS,
	        VISIBLE
    	};
    };
private:
    CV_EXPORTS static std::mutex instance_mtx_;
    CV_EXPORTS static thread_local cv::Ptr<V4D> instance_;
    CV_EXPORTS static thread_local ThreadSafeAnyMap<Keys::Enum> properties_;

    AllocateFlags::Enum allocateFlags_;
    ConfigFlags::Enum configFlags_;
    DebugFlags::Enum  debugFlags_;

    int samples_;
    cv::Ptr<FrameBufferContext> mainFbContext_ = nullptr;
    cv::Ptr<SourceContext> sourceContext_ = nullptr;
    cv::Ptr<SinkContext> sinkContext_ = nullptr;
    cv::Ptr<NanoVGContext> nvgContext_ = nullptr;
    cv::Ptr<BgfxContext> bgfxContext_ = nullptr;
    cv::Ptr<ImGuiContextImpl> imguiContext_ = nullptr;
    cv::Ptr<PlainContext> plainContext_ = nullptr;
    std::mutex glCtxMtx_;
    std::map<int32_t,cv::Ptr<GLContext>> glContexts_;
    std::map<int32_t,cv::Ptr<ExtContext>> extContexts_;
    bool closed_ = false;
    cv::Ptr<Source> source_;
    cv::Ptr<Sink> sink_;
    bool showFPS_ = true;
    bool printFPS_ = false;
    bool showTracking_ = true;
    std::string currentID_;
public:
    CV_EXPORTS static cv::Ptr<V4D> instance() {
    	std::lock_guard guard(instance_mtx_);
    	if(!instance_)
    		CV_Error(cv::Error::StsAssert, "Runtime not initialized. You have to call the static ```init``` function of the runtime first!");
    	return instance_;
    }

    static void init_keys() {
        auto fb = std::dynamic_pointer_cast<FrameBufferContext>(instance_->fbCtx());
        create<true>(Keys::SIZE, fb->size());
        create<false>(Keys::VIEWPORT, cv::Rect(0,0,fb->size().width, fb->size().height));
        create<false, cv::Size>(Keys::WINDOW_SIZE, fb->size(), [](const cv::Size& sz){ std::dynamic_pointer_cast<FrameBufferContext>(V4D::instance()->fbCtx())->setWindowSize(sz); });
        create<true>(Keys::FRAMEBUFFER_SIZE, fb->size());
        create<false>(Keys::CLEAR_COLOR, cv::Scalar(0, 0, 0, 255));
        create<false,string>(Keys::NAMESPACE, "default");
        create<false, bool>(Keys::FULLSCREEN, false, [](const bool& fs){ std::dynamic_pointer_cast<FrameBufferContext>(V4D::instance()->fbCtx())->setFullscreen(fs); });
        create<false>(Keys::DISABLE_INPUT_EVENTS, false);
        create<false, bool>(Keys::VISIBLE, fb->isVisible(), [](const bool& v){ std::dynamic_pointer_cast<FrameBufferContext>(V4D::instance()->fbCtx())->setVisible(v); });
    }

    template<bool Tread, typename Tval>
    static void create(Keys::Enum key, const Tval& val, const std::function<void(const Tval& val)>& cb = std::function<void(const Tval& val)>()) {
        properties_.create<Tread>(key, val, cb);
    }

    template<typename Tval>
    static void set(Keys::Enum key, const Tval& val, bool fire = true) {
    	if(instance()->debugFlags() & DebugFlags::MONITOR_RUNTIME_PROPERTIES) {
    		stringstream ss;
    		ss << demangle(typeid(decltype(key)).name()) << " = " << size_t(&val) << " (fire: " << fire << ")";
    		CV_LOG_INFO(&mon_tag, ss.str());
    	}
    	properties_.set(key, val, fire);
	}

    template<typename Tval>
    static const auto& get(Keys::Enum key) {
		return properties_.get<Tval>(key);
	}

	template <typename V>
	static V apply(Keys::Enum k, std::function<V(V&)> f) {
		return properties_.apply(k, f);
	}

    CV_EXPORTS static cv::Ptr<V4D> init(const cv::Rect& viewport, const string& title, AllocateFlags::Enum allocateFlags = AllocateFlags::DEFAULT, ConfigFlags::Enum configFlags = ConfigFlags::DEFAULT, DebugFlags::Enum debugFlags = DebugFlags::DEFAULT, int samples = 0);
    CV_EXPORTS static cv::Ptr<V4D> init(const cv::Rect& viewport, const cv::Size& fbsize, const string& title, AllocateFlags::Enum allocateFlags = AllocateFlags::DEFAULT, ConfigFlags::Enum configFlags = ConfigFlags::DEFAULT, DebugFlags::Enum debugFlags = DebugFlags::DEFAULT, int samples = 0);
    CV_EXPORTS static cv::Ptr<V4D> init(const V4D& v4d, const string& title);

    CV_EXPORTS virtual ~V4D();
    CV_EXPORTS std::string title() const;
    CV_EXPORTS void copyTo(cv::UMat& arr);
    CV_EXPORTS void copyFrom(const cv::UMat& arr);
    CV_EXPORTS void setSource(cv::Ptr<Source> src);
    CV_EXPORTS cv::Ptr<Source> getSource();
    CV_EXPORTS bool hasSource() const;
    CV_EXPORTS void setSink(cv::Ptr<Sink> sink);
    CV_EXPORTS cv::Ptr<Sink> getSink();
    CV_EXPORTS bool hasSink() const;
    CV_EXPORTS cv::Vec2f position();
    CV_EXPORTS float pixelRatioX();
    CV_EXPORTS float pixelRatioY();
    CV_EXPORTS const cv::Size& size();
    CV_EXPORTS bool getShowFPS();
    CV_EXPORTS void setShowFPS(bool s);
    CV_EXPORTS bool getPrintFPS();
    CV_EXPORTS void setPrintFPS(bool p);
    CV_EXPORTS bool getShowTracking();
    CV_EXPORTS void setShowTracking(bool st);
    CV_EXPORTS void printSystemInfo();
    CV_EXPORTS AllocateFlags::Enum allocateFlags();
    CV_EXPORTS ConfigFlags::Enum configFlags();
    CV_EXPORTS DebugFlags::Enum debugFlags();

    // PlanRuntime implementation (defined in v4d.cpp)
    cv::Ptr<cv::plan::detail::PlainContext> plainCtx() override;
    cv::Ptr<cv::plan::detail::PlanContext> glCtx(int32_t idx = 0) override;
    cv::Ptr<cv::plan::detail::PlanContext> fbCtx() override;
    cv::Ptr<cv::plan::detail::PlanContext> nvgCtx() override;
    cv::Ptr<cv::plan::detail::PlanContext> bgfxCtx() override;
    cv::Ptr<cv::plan::detail::PlanContext> extCtx(int32_t idx = 0) override;
    cv::Ptr<cv::plan::detail::PlanContext> sourceCtx() override;
    cv::Ptr<cv::plan::detail::PlanContext> sinkCtx() override;
    cv::Ptr<cv::plan::detail::PlanContext> imguiCtx() override;

    bool hasPlainCtx() override;
    bool hasGlCtx(uint32_t idx = 0) override;
    bool hasFbCtx() override;
    bool hasNvgCtx() override;
    bool hasBgfxCtx() override;
    bool hasExtCtx(uint32_t idx = 0) override;
    bool hasSourceCtx() override;
    bool hasSinkCtx() override;
    bool hasImguiCtx() override;

    uint32_t debugFlagsVal() const { return static_cast<uint32_t>(debugFlags_); }
    uint32_t debugFlags() const override { return static_cast<uint32_t>(debugFlags_); }

    cv::Rect getViewport() const override {
        return get<cv::Rect>(Keys::VIEWPORT);
    }

    static void run(cv::Ptr<V4D> runtime, std::function<void()> runGraph) {
		static Resequence reseq(1);
    	static std::binary_semaphore frame_sync_render(0);
		static std::binary_semaphore frame_sync_sema_swap(0);

		try {
			if(GlobalState::isMain()) {
				CV_LOG_INFO(&v4d_tag, "Display thread started.");
				while(keep_running()) {
					bool result = true;
					TimeTracker::getInstance()->execute("display", [&result, runtime](){
					if(runtime->configFlags() & ConfigFlags::DISPLAY_MODE) {
						if(!runtime->display()) {
							frame_sync_render.release();
							result = false;
						}
						frame_sync_render.release();
						frame_sync_sema_swap.acquire();
					} else {
						if(!runtime->display()) {
							result = false;
						}
					}
					});
					if(!result)
						break;
				}
			} else {
				while(keep_running()) {
					bool result = true;
					TimeTracker::getInstance()->execute("worker", [&result, runtime, runGraph](){
						event::poll();
                        GlobalState::apply<size_t>(GlobalState::Keys::RUN_CNT, [runtime](size_t& s) {
                            ++s;
                            return s;
                        });

                        size_t seq = GlobalState::apply<size_t>(GlobalState::Keys::FRAME_CNT, [runtime](size_t& s) {
                            ++s;
                            return s;
                        });

						if(runtime->configFlags() & ConfigFlags::DISPLAY_MODE) {
							frame_sync_sema_swap.release();
							runGraph();
							reseq.waitFor(seq, [](uint64_t s) {
								CV_UNUSED(s);
								frame_sync_render.acquire();
							});

							if(!runtime->display()) {
								frame_sync_sema_swap.release();
								result = false;
							}
						} else {
							runGraph();
							reseq.waitFor(seq, [&result, runtime](uint64_t s) {
								CV_UNUSED(s);
								result = runtime->display();
							});
						}
					});
					if(!result)
						break;
				}
			}
		} catch(std::runtime_error& ex) {
			CV_LOG_WARNING(&v4d_tag, "Pipeline terminated: " << ex.what());
		} catch(std::exception& ex) {
			CV_LOG_WARNING(&v4d_tag, "Pipeline terminated: " << ex.what());
		} catch(...) {
			CV_LOG_WARNING(&v4d_tag, "Pipeline terminated with unknown error.");
		}
		request_finish();
		reseq.finish();
		if(runtime->configFlags() & ConfigFlags::DISPLAY_MODE) {
			if(GlobalState::isMain()) {
				for(size_t i = 0; i < GlobalState::get<size_t>(GlobalState::Keys::WORKERS_STARTED); ++i)
					frame_sync_render.release();
			} else {
				frame_sync_sema_swap.release();
			}
    	}
    }
private:
    V4D(const V4D& v4d, const string& title);
    V4D(const cv::Rect& size, cv::Size fbsize,
            const string& title, AllocateFlags::Enum allocFlags, ConfigFlags::Enum confFlags, DebugFlags::Enum debFlags, int samples);

    void swapContextBuffers();
    bool display();
    GLFWwindow* getGLFWWindow() const;
    bool isFocused();
    void setFocused(bool f);
};

class CV_EXPORTS V4DPlan : public Plan {
	friend class SharedVariables;

public:
	template<typename TeventClass, typename Tfn = std::function<std::vector<std::shared_ptr<TeventClass>>()>, typename Tparent = Edge<Tfn, false, true, false>>
	struct Event : Tparent {
		Event(cv::Ptr<Plan> plan) : Tparent(Tparent::make(plan, wrap_callable<>([]() {
			if(!V4D::instance()->get<bool>(V4D::Keys::DISABLE_INPUT_EVENTS))
				return gwe::fetch<TeventClass>();
			else
				return typename TeventClass::List();
		}))) {
			static_assert(Tparent::func_t::value, "Internal error: Function not recognized!");
		}

		Event(cv::Ptr<Plan> plan, const typename TeventClass::Type t) : Tparent(Tparent::make(plan, wrap_callable<>([t]() {
			if(!V4D::instance()->get<bool>(V4D::Keys::DISABLE_INPUT_EVENTS))
				return gwe::fetch<TeventClass>(t);
			else
				return typename TeventClass::List();
		}))) {
			static_assert(Tparent::func_t::value, "Internal error: Function not recognized!");
		}

		template<typename Ttrigger>
		Event(cv::Ptr<Plan> plan, const typename TeventClass::Type t, const Ttrigger tr) : Tparent(Tparent::make(plan, wrap_callable<>([t, tr]() {
			if(!V4D::instance()->get<bool>(V4D::Keys::DISABLE_INPUT_EVENTS))
				return gwe::fetch<TeventClass>(t, tr);
			else
				return typename TeventClass::List();
		}))) {
			static_assert(Tparent::func_t::value, "Internal error: Function not recognized!");
		}
	};

	V4DPlan() : Plan() {
		runtime_ = V4D::instance();
	}

    template <typename Tedge>
    typename std::enable_if<std::is_base_of_v<EdgeBase, Tedge>, cv::Ptr<V4DPlan>>::type
    branch(Tedge edge) {
        Plan::branch(edge);
        return self<V4DPlan>();
    }

    template <typename Tfn>
    typename std::enable_if<!std::is_base_of_v<EdgeBase, Tfn>, cv::Ptr<V4DPlan>>::type
    branch(Tfn fn) {
        Plan::branch(fn);
        return self<V4DPlan>();
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_integral_v<Tfn>, cv::Ptr<V4DPlan>>::type
    branch(Tfn fn, Args ... args) {
        Plan::branch(fn, args...);
        return self<V4DPlan>();
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of<EdgeBase, Tfn>::value, cv::Ptr<V4DPlan>>::type
    branch(int workerIdx, Tfn fn, Args ... args) {
        Plan::branch(workerIdx, fn, args...);
        return self<V4DPlan>();
    }

    cv::Ptr<V4DPlan> branch(int workerIdx, BranchType::Enum type, std::function<bool()> fn) {
        Plan::branch(workerIdx, type, fn);
        return self<V4DPlan>();
    }

    template <typename Tedge>
    typename std::enable_if<std::is_base_of_v<EdgeBase, Tedge>, cv::Ptr<V4DPlan>>::type
    branch(BranchType::Enum type, Tedge edge) {
        Plan::branch(type, edge);
        return self<V4DPlan>();
    }

    template <typename Tfn, typename ... Args>
    cv::Ptr<V4DPlan> branch(BranchType::Enum type, Tfn fn, Args ... args) {
        Plan::branch(type, fn, args...);
        return self<V4DPlan>();
    }

    cv::Ptr<V4DPlan> endBranch() {
        Plan::endBranch();
        return self<V4DPlan>();
    }

    cv::Ptr<V4DPlan> elseBranch() {
        Plan::elseBranch();
        return self<V4DPlan>();
    }

    template <typename ... Args>
    cv::Ptr<V4DPlan> plain(Args... args) {
        Plan::plain(args...);
        return self<V4DPlan>();
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of<EdgeBase, Tfn>::value, cv::Ptr<V4DPlan>>::type
    plain(Tfn fn, Args... args) {
        Plan::plain(fn, args...);
        return self<V4DPlan>();
    }

    template <typename TsubPlan>
    cv::Ptr<V4DPlan> subInfer(cv::Ptr<TsubPlan> subPlan) {
        Plan::subInfer(subPlan);
        return self<V4DPlan>();
    }

    template <typename TsubPlan>
    cv::Ptr<V4DPlan> subSetup(cv::Ptr<TsubPlan> subPlan) {
        Plan::subSetup(subPlan);
        return self<V4DPlan>();
    }

    template <typename TsubPlan>
    cv::Ptr<V4DPlan> subTeardown(cv::Ptr<TsubPlan> subPlan) {
        Plan::subTeardown(subPlan);
        return self<V4DPlan>();
    }

    template<typename ... Edges>
    cv::Ptr<V4DPlan> op(Edges ... edges){
        Plan::op(edges...);
        return self<V4DPlan>();
    }

    template<typename ... Edges>
    cv::Ptr<V4DPlan> assign(Edges ... edges){
        Plan::assign(edges...);
        return self<V4DPlan>();
    }

    template<typename ... Edges>
    cv::Ptr<V4DPlan> construct(Edges ... edges){
        Plan::construct(edges...);
        return self<V4DPlan>();
    }

    template <typename Tfn, typename ... Args>
    void imgui(Tfn fn, Args&& ... args) {
        auto argsTuple = std::make_tuple(args...);
        auto wrap = wrapGuiCall(fn, argsTuple, std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
        cv::Ptr<Transaction> tx = make_transaction(wrap, args...);
                if(runtime()->hasImguiCtx())
        	std::dynamic_pointer_cast<ImGuiContextImpl>(runtime()->imguiCtx())->setTransaction(tx);
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of<EdgeBase, Tfn>::value, cv::Ptr<V4DPlan>>::type
    gl(Tfn fn, Args ... args) {
    	auto argsTuple = std::make_tuple(args...);
    	call(runtime()->glCtx(-1), "gl", fn, std::forward<decltype(argsTuple)>(argsTuple), std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
    	return self<V4DPlan>();
    }

    template <int32_t pos = 0, typename Tedge, typename Tfn, typename ... Args>
    typename std::enable_if<std::is_base_of<EdgeBase, Tedge>::value, cv::Ptr<V4DPlan>>::type
	gl(Tedge indexEdge, Tfn fn, Args ... args) {
        auto ctxCallback = [this, indexEdge]() {
			Tedge copy = indexEdge;
			return runtime()->glCtx(copy.ref());};
		auto argsTuple = std::make_tuple(args...);
		if constexpr(pos > 0) {
			auto beforePos = sub_tuple<0,pos>(argsTuple);
			auto afterPos = sub_tuple<pos, sizeof...(args) - pos>(argsTuple);
			auto allTuple = std::tuple_cat(beforePos, indexEdge, afterPos);
			call(ctxCallback, "gl-i", fn, std::forward<decltype(allTuple)>(allTuple), std::make_index_sequence<std::tuple_size<decltype(allTuple)>::value>());
		} else if constexpr(pos < 0) {
			call(ctxCallback, "gl-i", fn, std::forward<decltype(argsTuple)>(argsTuple), std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
		} else {
			auto allTuple = std::make_tuple(indexEdge, args...);
			call(ctxCallback, "gl-i", fn, std::forward<decltype(allTuple)>(allTuple), std::make_index_sequence<std::tuple_size<decltype(allTuple)>::value>());
		}
		return self<V4DPlan>();
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of<EdgeBase, Tfn>::value, cv::Ptr<V4DPlan>>::type
    ext(Tfn fn, Args ... args) {
    	auto argsTuple = std::make_tuple(args...);
    	call(runtime()->extCtx(-1), "ext", fn, std::forward<decltype(argsTuple)>(argsTuple), std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
    	return self<V4DPlan>();
    }

    template <int32_t pos = 0, typename Tedge, typename Tfn, typename ... Args>
    typename std::enable_if<std::is_base_of<EdgeBase, Tedge>::value, cv::Ptr<V4DPlan>>::type
	ext(Tedge indexEdge, Tfn fn, Args ... args) {
        auto ctxCallback = [this, indexEdge]() {
			Tedge copy = indexEdge;
			return runtime()->extCtx(copy.ref());};
		auto argsTuple = std::make_tuple(args...);
		if constexpr(pos > 0) {
			auto beforePos = sub_tuple<0,pos>(argsTuple);
			auto afterPos = sub_tuple<pos, sizeof...(args) - pos>(argsTuple);
			auto allTuple = std::tuple_cat(beforePos, indexEdge, afterPos);
			call(ctxCallback, "ext-i", fn, std::forward<decltype(allTuple)>(allTuple), std::make_index_sequence<std::tuple_size<decltype(allTuple)>::value>());
		} else if constexpr(pos < 0) {
			call(ctxCallback, "ext-i", fn, std::forward<decltype(argsTuple)>(argsTuple), std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
		} else {
			auto allTuple = std::make_tuple(indexEdge, args...);
			call(ctxCallback, "ext-i", fn, std::forward<decltype(allTuple)>(allTuple), std::make_index_sequence<std::tuple_size<decltype(allTuple)>::value>());
		}
		return self<V4DPlan>();
    }

    cv::Ptr<V4DPlan> clear(const int32_t& glIndex = -1) {
        gl<-1>(V(glIndex), [](const cv::Scalar& bgra){
            const float& b = bgra[0] / 255.0f;
		    const float& g = bgra[1] / 255.0f;
		    const float& r = bgra[2] / 255.0f;
		    const float& a = bgra[3] / 255.0f;
            GL_CHECK(glClearColor(r, g, b, a));
		    GL_CHECK(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
    	}, P<cv::Scalar>(V4D::Keys::CLEAR_COLOR));
    	return self<V4DPlan>();
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of_v<EdgeBase, Tfn>, cv::Ptr<V4DPlan>>::type
    capture(Tfn fn, Args... args) {
        auto srcEdge = makeInternalEdge<false>(std::dynamic_pointer_cast<SourceContext>(runtime()->sourceCtx())->sourceBuffer());
    	auto argsTuple = std::make_tuple(srcEdge, args...);
    	call(runtime()->sourceCtx(), "src", fn, std::forward<decltype(argsTuple)>(argsTuple), std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
    	return self<V4DPlan>();
    }

    template <typename Tedge>
    cv::Ptr<V4DPlan> capture(Tedge&& edge) {
        capture([](const cv::UMat& inputFrame, cv::UMat& f){
    	    if(!inputFrame.empty())
    			inputFrame.copyTo(f);
    	}, edge);

		return self<V4DPlan>();
    }

    cv::Ptr<V4DPlan> capture() {
        capture([](const cv::UMat& inputFrame, cv::UMat& f){
            if(!inputFrame.empty())
                inputFrame.copyTo(f);
        }, Edge<cv::UMat, false, false>::make(self<V4DPlan>(), captureFrame_));

        fb([](cv::UMat& framebuffer, const cv::UMat& f) {
            if(!f.empty()) {
                    cv::resize(f, framebuffer, framebuffer.size(), INTER_LINEAR);
            }
        }, Edge<cv::UMat, false, true>::make(self<V4DPlan>(), captureFrame_));

        return self<V4DPlan>();
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of_v<EdgeBase, Tfn>, cv::Ptr<V4DPlan>>::type
    write(Tfn fn, Args ... args) {
        if(!getParentID().empty())
            return self<V4DPlan>();

        using Tfb = typename std::tuple_element<0, typename function_traits<Tfn>::argument_types>::type;
		static_assert((std::is_same<Tfb,cv::UMat>::value) || !"The first argument must be of type 'cv::UMat&'");
		auto sinkEdge = makeInternalEdge<std::is_const<Tfb>::value>(std::dynamic_pointer_cast<SinkContext>(runtime()->sinkCtx())->sinkBuffer());

		auto argsTuple = std::make_tuple(sinkEdge, args...);
		call(runtime()->sinkCtx(), "sink", fn, std::forward<decltype(argsTuple)>(argsTuple), std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
		return self<V4DPlan>();
    }

    template<typename Tedge>
    cv::Ptr<V4DPlan> write(Tedge&& edge) {
        if(!getParentID().empty())
            return self<V4DPlan>();

        write([](cv::UMat& outputFrame, const cv::UMat& f){
            f.copyTo(outputFrame);
        }, edge);
        return self<V4DPlan>();
    }

    cv::Ptr<V4DPlan> write() {
        if(!getParentID().empty())
            return self<V4DPlan>();

        auto writerEdge = makeInternalEdge<false>(writerFrame_);
    	auto writerEdgeConst = makeInternalEdge<true>(writerFrame_);

        fb([](const cv::UMat& framebuffer, cv::UMat& f) {
            framebuffer.copyTo(f);
        }, writerEdge);

     	write([](cv::UMat& outputFrame, const cv::UMat& f){
   			f.copyTo(outputFrame);
    	}, writerEdgeConst);
		return self<V4DPlan>();
    }

    template <typename Tfn, typename ... Args>
    cv::Ptr<V4DPlan> nvg(Tfn fn, Args... args) {
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);

        const string id = make_id(this->space(), "nvg", fn, args...);
        emit_access(id, R(*this));
        (emit_access(id, args ),...);
		add_transaction(runtime()->nvgCtx(), id, wrap, args...);
		return self<V4DPlan>();
    }

    template <typename Tfn, typename ... Args>
    cv::Ptr<V4DPlan> bgfx(Tfn fn, Args... args) {
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);

        const string id = make_id(this->space(), "bgfx", fn, args...);
        emit_access(id, R(*this));
        (emit_access(id, args ),...);
		add_transaction(runtime()->bgfxCtx(), id, wrap, args...);
		return self<V4DPlan>();
    }

    template <size_t pos = 0, typename Tfn, typename ... Args>
    cv::Ptr<V4DPlan> fb(Tfn fn, Args ... args) {
		using isMemFn = typename CallableTraits<Tfn>::member_t;
		constexpr size_t idx = pos > 0 && isMemFn::value ? pos - 1 : pos;
		using Tfb = typename std::tuple_element<idx, typename function_traits<Tfn>::argument_types>::type;
		auto argsTuple = std::make_tuple(args...);
		if constexpr(pos > 0) {
			auto beforeFb = sub_tuple<0,pos>(argsTuple);
			auto afterFb = sub_tuple<pos, sizeof...(args) - pos>(argsTuple);
			auto fbEdge = std::make_tuple(makeInternalEdge<std::is_const<Tfb>::value>(std::dynamic_pointer_cast<FrameBufferContext>(runtime()->fbCtx())->fb()));
			auto allTuple = std::tuple_cat(beforeFb, fbEdge, afterFb);
			call(runtime()->fbCtx(), "fb", fn, std::forward<decltype(allTuple)>(allTuple), std::make_index_sequence<std::tuple_size<decltype(allTuple)>::value>());
		} else {
			auto allTuple = std::make_tuple(makeInternalEdge<std::is_const<Tfb>::value>(std::dynamic_pointer_cast<FrameBufferContext>(runtime()->fbCtx())->fb()), args...);
			call(runtime()->fbCtx(), "fb", fn, std::forward<decltype(allTuple)>(allTuple), std::make_index_sequence<std::tuple_size<decltype(allTuple)>::value>());
		}
		return self<V4DPlan>();
    }

    template<typename Tedge>
    cv::Ptr<V4DPlan> set(const V4D::Keys::Enum& key, const Tedge& e) {
        auto plan = self<V4DPlan>();
        using TE = std::remove_const_t<Tedge>;
        using ref_t = decltype(std::declval<TE>().ref());
        std::function<void(ref_t)> fn = [plan, key](ref_t v){
            V4D::set(key, v);
        };
        const string id = make_id(this->space(), "set-fn", fn, e);
        emit_access(id, R(*this));
        emit_access(id, e);
        add_transaction(runtime()->plainCtx(), id, fn, e);
        return self<V4DPlan>();
    }

    template<typename ... Args>
    cv::Ptr<V4DPlan> set(std::tuple<V4D::Keys::Enum,Args>&& ... tuples) {
        (set(std::forward<std::tuple<V4D::Keys::Enum,Args>>(tuples)),...);
        return self<V4DPlan>();
    }

    template<typename Tval>
    Property<Tval> P(V4D::Keys::Enum key) {
        const auto& ref = std::dynamic_pointer_cast<V4D>(runtime())->template get<Tval>(key);
        return Property<Tval>(self<Plan>(), ref);
    }

    template<typename Tval>
    Property<Tval> P(LocalState::Keys::Enum key) {
        const auto& ref = LocalState::get<Tval>(key);
        return Property<Tval>(self<Plan>(), ref);
    }

    template<typename Tval>
    Property<Tval> P(GlobalState::Keys::Enum key) {
        const auto& ref = GlobalState::get<Tval>(key);
        return Property<Tval>(self<Plan>(), ref);
    }

    template<typename Tclass>
    Event<Tclass> E() {
        return Event<Tclass>(self<Plan>());
    }

    template<typename Tclass>
    Event<Tclass> E(typename Tclass::Type t) {
        return Event<Tclass>(self<Plan>(), t);
    }

    template<typename Tclass, typename Ttrigger>
    Event<Tclass> E(typename Tclass::Type t, Ttrigger tr) {
        return Event<Tclass>(self<Plan>(), t, tr);
    }

    template<typename Tplan, typename ... Args>
    static cv::Ptr<Tplan> make(Args&& ... args) {
    	Tplan* plan = new Tplan(std::forward<Args>(args)...);
    	plan->template setActualTypeSize<Tplan>();
    	V4D::set(V4D::Keys::NAMESPACE, plan->space());
		return plan->template self<Tplan>();
    }

    template<typename Tplan, typename ... Args>
	static void run(int32_t workers, Args&& ... args) {
		CV_Assert(workers > -2);
		if(workers == -1) {
			workers = 2;
		} else {
			++workers;
		}

		cv::Ptr<Tplan> plan;
        static std::mutex worker_init_mtx_;

		std::vector<std::thread*> threads;
		{
			static std::mutex runMtx;
			std::lock_guard<std::mutex> lock(runMtx);
			cv::setNumThreads(0);

			if(GlobalState::isFirstRun()) {
				GlobalState::setMainID(std::this_thread::get_id());
				CV_LOG_INFO(&v4d_tag, "Starting with " << workers << " workers");
			}

	    	plan = V4DPlan::make<Tplan>(std::forward<Args>(args)...);

			if(GlobalState::isMain()) {
				const string title = V4D::instance()->title();
				auto src = V4D::instance()->getSource();
				auto sink = V4D::instance()->getSink();

				if(!(V4D::instance()->debugFlags() & DebugFlags::DONT_PAUSE_LOG)) {
					CV_LOG_WARNING(&v4d_tag, "Temporary setting log level to warning.");
					cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_WARNING);
				}

                GlobalState::set<size_t>(GlobalState::Keys::WORKERS_STARTED, workers);
				for (int32_t i = 0; i < workers; ++i) {
					threads.push_back(
						new std::thread(
							[plan, i, src, sink, &args...] {
								auto v4d = std::dynamic_pointer_cast<V4D>(plan->runtime());
								string name = v4d->title() + "-" + std::to_string(i);
					            setThreadName(name.c_str());
					            CV_LOG_DEBUG(&v4d_tag, "Creating worker: " << name);
								cv::Ptr<V4D> worker;
								{
									std::lock_guard guard(worker_init_mtx_);
									worker = V4D::init(*v4d.get(), name);

									if (src) {
										worker->setSource(src);
									}
									if (sink) {
										worker->setSink(sink);
									}
								}

								LocalState::set(LocalState::Keys::WORKER_INDEX, size_t(i));
								V4DPlan::run<Tplan>(0, std::forward<Args>(args)...);
							}
						)
					);
				}
			} else {
				if(V4D::instance()->debugFlags() & DebugFlags::LOWER_WORKER_PRIORITY) {
#if defined(__linux__)
					CV_LOG_INFO(&v4d_tag, "Lowering worker thread niceness from: " << getpriority(PRIO_PROCESS, gettid()) << " to: " << 1);

					if (setpriority(PRIO_PROCESS, gettid(), 1)) {
						CV_LOG_INFO(&v4d_tag, "Failed to set niceness: " << std::strerror(errno));
					}
#endif
				}
			}
		}

		CV_Assert(plan);

		if(GlobalState::isMain()) {
			V4D::instance()->printSystemInfo();
            CV_LOG_WARNING(&v4d_tag, "Setting loglevel to INFO");
            cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_INFO);
            CV_LOG_INFO(&v4d_tag, "Starting pipelines with " << GlobalState::get<size_t>(GlobalState::Keys::WORKERS_STARTED) << " workers.");
		} else {
			static std::binary_semaphore setup_sema(1);
			try {
				CV_LOG_DEBUG(&v4d_tag, "Setup on worker: " << LocalState::get<size_t>(LocalState::Keys::WORKER_INDEX));
				setup_sema.acquire();
				plan->setup();
				plan->makeGraph();
				plan->runGraph();
				plan->clearGraph();
				setup_sema.release();
			} catch(std::exception& ex) {
				CV_Error_(cv::Error::StsError, ("Setup failed: %s", ex.what()));
			}
			CV_LOG_DEBUG(&v4d_tag, "Setup finished: " << LocalState::get<size_t>(LocalState::Keys::WORKER_INDEX));
		}
		if(GlobalState::isMain()) {
			try {
				CV_LOG_DEBUG(&v4d_tag, "Loading GUI");
				V4D::set(V4D::Keys::NAMESPACE, plan->space());
				plan->gui();
			} catch(std::exception& ex) {
				CV_Error_(cv::Error::StsError, ("Loading GUI failed: %s", ex.what()));
			}
		} else {
			try {
				CV_LOG_DEBUG(&v4d_tag, "Main inference on worker: " << LocalState::get<size_t>(LocalState::Keys::WORKER_INDEX));
				plan->infer();
				plan->makeGraph();
			} catch(std::exception& ex) {
				CV_Error_(cv::Error::StsError, ("Main inference failed: %s", ex.what()));
			}
			CV_LOG_DEBUG(&v4d_tag, "Main inference finished: " << LocalState::get<size_t>(LocalState::Keys::WORKER_INDEX));
            GlobalState::apply<size_t>(GlobalState::Keys::WORKERS_READY, [](size_t& wr){ ++wr; return wr; });
		}
        static std::barrier syncPoint(std::ptrdiff_t(workers + 1));
        syncPoint.arrive_and_wait();

        if(GlobalState::isMain()) {
                    CV_LOG_INFO(&v4d_tag, "Starting pipelines with " << GlobalState::get<size_t>(GlobalState::Keys::WORKERS_STARTED) << " workers.");
        }

        try {
			V4D::run(V4D::instance(), [plan](){
				TimeTracker::getInstance()->execute("iteration", [plan](){
					plan->runGraph();
					GL_CHECK(glFlush());
				});
			});
		} catch(std::exception& ex) {
			CV_Error_(cv::Error::StsError, ("Main plan->runtime_: %s", ex.what()));
		}
		CV_LOG_DEBUG(&v4d_tag, "Main plan->runtime_ finished: " << LocalState::get<size_t>(LocalState::Keys::WORKER_INDEX));

		if(!GlobalState::isMain()) {
			plan->clearGraph();
			CV_LOG_DEBUG(&v4d_tag, "Starting teardown on worker: " << LocalState::get<size_t>(LocalState::Keys::WORKER_INDEX));
			try {
				plan->teardown();
				plan->makeGraph();
				plan->runGraph();
				plan->clearGraph();
			} catch(std::exception& ex) {
				CV_Error_(cv::Error::StsError, ("pipeline teardown failed: %s", ex.what()));
			}
			auto v4d = std::dynamic_pointer_cast<V4D>(plan->runtime());
			v4d->setSink(nullptr);
			v4d->setSource(nullptr);
			CV_LOG_DEBUG(&v4d_tag, "Teardown complete on worker: " << LocalState::get<size_t>(LocalState::Keys::WORKER_INDEX));
		} else {
			for(auto& t : threads)
				t->join();
			V4D::instance()->setSink(nullptr);
			V4D::instance()->setSource(nullptr);
			CV_LOG_INFO(&v4d_tag, "All threads terminated.");
		}
    }

protected:
    virtual void pf(const size_t& depth, const BranchState& current, const cv::Ptr<cv::plan::Node> n) override {
    	if(DebugFlags::PRINT_CONTROL_FLOW & V4D::instance()->debugFlags()) {
    		std::stringstream indent;
			indent << "|";
    		for(size_t i = 0; i < depth; ++i) {
				indent << "  ";
			}

    		std::stringstream ss;
			string name = n->name_;
			size_t offset = name.find_first_of(':', 0) + 1;

			size_t open = 0;
			size_t close = 0;
			while(true) {
				open = name.find_first_of('[', offset);
				if(open == string::npos)
					break;
				close = name.find_first_of(']', open);
				if(close == string::npos)
					break;
				CV_Assert(name.size() > close + 1);
				name.erase(open, close + 1 - open);
			}

			open = 0;
			close = 0;
			while(true) {
				open = name.find_first_of('(', offset);
				if(open == string::npos)
					break;
				close = name.find_first_of(')', open);
				if(close == string::npos)
					break;
				CV_Assert(name.size() > close + 1);
				name.erase(open, close + 1 - open);
			}

			ss << indent.str() << name;
			const string formattedName = ss.str();
			ss.str("");
			ss << indent.str() << "-> (enabled: " << current.isEnabled_ << ") "
					<< "(once: " << current.isOnce_ << ") "
					<< "(single: " << current.isSingle_ << ") "
					<< "(branch lock: " << current.isLocked_ << ") "
					<< "(shared lock: " << n->tx_->hasLockies() << ")";
			const string formattedInfo = ss.str();
			static std::mutex printMtx;
			std::lock_guard guard(printMtx);
			CV_LOG_INFO(&cf_tag, formattedName);
			CV_LOG_INFO(&cf_tag, formattedInfo);
    	}
    }

    virtual void runGraph() override {
		BranchType::Enum btype;
    	BranchState currentState;
    	bool countLockContention = DebugFlags::PRINT_LOCK_CONTENTION & V4D::instance()->debugFlags();
    	try {
			for (auto& n : currentNodes_) {
				btype = n->tx_->getBranchType();
				bool isBranch = n->name_.substr(0, 6) == "branch";
				bool isElse = n->name_.substr(0,6) == "[else]";
				bool isEnd = n->name_.substr(0,5) == "[end]";
				bool isElseIf = n->name_.substr(0,8) == "[elseif]";
				if(btype != BranchType::NONE) {
					CV_Assert((((isBranch != isElse) != isEnd) != isElseIf));
					if(isBranch) {
						if(!branchStateStack_.empty())
							currentState = branchStateStack_.front();
						else
							currentState = BranchState();
						currentState.branchID_ = n->name_;

						if(currentState.isEnabled_) {
							currentState.isOnce_ = ((btype == BranchType::ONCE) || (btype == BranchType::PARALLEL_ONCE));
							currentState.isSingle_ = ((btype == BranchType::ONCE) || (btype == BranchType::SINGLE));
						} else {
							currentState.isOnce_ = false;
							currentState.isSingle_ = false;
							currentState.isEnabled_ = false;
						}

						if(currentState.isEnabled_) {
							if(currentState.isOnce_) {
								if((btype == BranchType::ONCE)) {
									currentState.condition_ = GlobalState::once(n->name_) && n->tx_->performPredicate();
								} else if((btype == BranchType::PARALLEL_ONCE)) {
									currentState.condition_ = !n->tx_->ran() && n->tx_->performPredicate();
								} else {
									CV_Assert(false);
								}
							} else {
								currentState.condition_ = n->tx_->performPredicate();
							}

							currentState.isEnabled_ = currentState.isEnabled_ && currentState.condition_;

							if(currentState.isEnabled_ && currentState.isSingle_) {
								CV_Assert(btype != BranchType::PARALLEL);
								GlobalState::lockNode(currentState.branchID_);
								currentState.isLocked_ = true;
							}
						}
						branchStateStack_.push_front(currentState);
						pf(branchStateStack_.size(), currentState, n);
					} else if(isElse) {
						if(branchStateStack_.empty())
							continue;
						currentState = branchStateStack_.front();
						currentState.isEnabled_ = !currentState.condition_;
						currentState.isOnce_ = false;
						currentState.condition_ = !currentState.condition_;
						currentState.isSingle_ = false;

						if(currentState.isLocked_) {
						    GlobalState::tryUnlockNode(currentState.branchID_);
						}

						currentState.isLocked_ = false;
						pf(branchStateStack_.size(), currentState, n);
						branchStateStack_.pop_front();
						branchStateStack_.push_front(currentState);
					} else if(isEnd) {
						if(branchStateStack_.empty())
							continue;

						currentState = branchStateStack_.front();
						GlobalState::tryUnlockNode(currentState.branchID_);
						pf(branchStateStack_.size(), currentState, n);
						branchStateStack_.pop_front();
					} else {
						CV_Assert(false);
					}
				} else {
					CV_Assert(!n->tx_->isPredicate());
					currentState = !branchStateStack_.empty() ? branchStateStack_.front() : BranchState();
					if(currentState.isEnabled_) {
						auto lock = GlobalState::tryGetNodeLock(currentState.branchID_);
						auto plan = self<Plan>();
						auto ctx = n->tx_->getContextCallback()();
						auto viewport = runtime()->getViewport();

						if(lock)
						{
							std::lock_guard<std::mutex> guard(*lock.get());
							int res = ctx->execute(viewport, [plan, countLockContention, n,currentState]() {
								TimeTracker::getInstance()->execute(n->name_, [plan, countLockContention, n,currentState](){
									n->tx_->perform();
								});
							});
							if(res <= 0) {
                                CV_LOG_WARNING(&v4d_tag, "Context failed while: " + n->name_);
							}
						} else {
							int res = ctx->execute(viewport, [plan, countLockContention, n,currentState]() {
								TimeTracker::getInstance()->execute(n->name_, [plan, countLockContention, n,currentState](){
									n->tx_->perform();
								});
							});
							if(res <= 0) {
								CV_LOG_WARNING(&v4d_tag, "Context failed while: " + n->name_);
							}
						}
					}
					pf(branchStateStack_.size() +1 , currentState, n);
					currentState = BranchState();
				}
			}

			size_t lockCnt = GlobalState::countNodeLocks();
			CV_Assert(branchStateStack_.empty());
			CV_Assert(lockCnt == 0);
    	} catch(std::runtime_error& ex) {
			if(!branchStateStack_.empty() && branchStateStack_.front().isLocked_) {
				if(GlobalState::tryUnlockNode(currentState.branchID_)) {
				}
			}
			throw ex;
		} catch(std::exception& ex) {
			if(!branchStateStack_.empty() && branchStateStack_.front().isLocked_) {
				if(GlobalState::tryUnlockNode(currentState.branchID_)) {
				}
			}
			throw ex;
		} catch(...) {
			if(!branchStateStack_.empty() && branchStateStack_.front().isLocked_) {
				if(GlobalState::tryUnlockNode(currentState.branchID_)) {
				}
			}
			throw std::runtime_error("Unkown error.");
		}
	}
};

} /* namespace v4d */
} /* namespace cv */

#endif /* SRC_OPENCV_V4D_V4D_HPP_ */
