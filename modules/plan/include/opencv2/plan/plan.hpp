// This file is part of OpenCV project. It is subject to the license terms
// in the LICENSE file found in the top-level directory of this distribution.
//
// Plan extracted from the former v4d module. V4D has been abandoned: Plan now
// sits on top of a small cv::plan::Runtime that provides the property store,
// the source/sink/framebuffer buffers and a pluggable context registry.
#ifndef OPENCV_PLAN_PLAN_HPP_
#define OPENCV_PLAN_PLAN_HPP_

#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>

#include "util.hpp"
#include "threadsafeanymap.hpp"
#include "source.hpp"
#include "sink.hpp"
#include "events.hpp"

#include "detail/context.hpp"
#include "detail/framebuffercontext.hpp"
#include "detail/sourcecontext.hpp"
#include "detail/sinkcontext.hpp"
#include "detail/transaction.hpp"
#include "detail/timetracker.hpp"
#include "detail/resequence.hpp"

#include <shared_mutex>
#include <future>
#include <set>
#include <map>
#include <string>
#include <memory>
#include <vector>
#include <barrier>
#include <semaphore>
#include <type_traits>
#include <thread>
#include <sstream>
#include <iomanip>

namespace cv {
namespace plan {

using namespace cv::plan::detail;

namespace detail {

template<typename T> std::string int_to_hex(T i) {
    std::stringstream stream;
    stream << "0x" << std::setfill('0') << std::setw(sizeof(T) * 2) << std::hex << i;
    return stream.str();
}

template<typename Tlamba> std::string lambda_ptr_hex(Tlamba&& l) {
    return int_to_hex((size_t)Lambda::ptr(l));
}

static std::size_t map_index(const std::thread::id id) {
    static std::size_t nextindex = 0;
    static std::mutex my_mutex;
    static std::unordered_map<std::thread::id, std::size_t> ids;
    std::lock_guard<std::mutex> lock(my_mutex);
    auto iter = ids.find(id);
    if (iter == ids.end())
        return ids[id] = nextindex++;
    return iter->second;
}

} // namespace detail

/*!
 * Runtime replaces the former V4D object. It owns the framebuffer/source/sink
 * buffers, the plain context and a registry of named (optionally indexed)
 * contexts. Anything V4D-specific (GL windows, NanoVG, ImGui, bgfx) is gone.
 */
class CV_EXPORTS Runtime {
    friend class Plan;
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
    CV_EXPORTS static thread_local cv::Ptr<Runtime> instance_;
    CV_EXPORTS static thread_local ThreadSafeAnyMap<Keys::Enum> properties_;

    cv::Size size_;
    std::string title_;

    cv::Ptr<detail::FrameBufferContext> fbContext_;
    cv::Ptr<detail::SourceContext> sourceContext_;
    cv::Ptr<detail::SinkContext> sinkContext_;
    cv::Ptr<detail::PlainContext> plainContext_;

    std::mutex ctxMtx_;
    std::map<std::pair<string, int32_t>, cv::Ptr<detail::PlanContext>> contexts_;

    cv::Ptr<Source> source_;
    cv::Ptr<Sink> sink_;
    std::function<bool()> displayCallback_;

    Runtime(const cv::Size& size, const string& title);
    Runtime(const Runtime& other, const string& title);

public:
    CV_EXPORTS virtual ~Runtime();

    CV_EXPORTS static cv::Ptr<Runtime> instance() {
        std::lock_guard guard(instance_mtx_);
        if (!instance_)
            CV_Error(cv::Error::StsAssert,
                     "Runtime not initialized. You have to call the static `init` function first!");
        return instance_;
    }

    CV_EXPORTS static cv::Ptr<Runtime> init(const cv::Size& size, const string& title);
    CV_EXPORTS static cv::Ptr<Runtime> init(const Runtime& other, const string& title);

    static void init_keys() {
        create<true>(Keys::SIZE, instance_->size());
        create<false>(Keys::VIEWPORT, cv::Rect(0, 0, instance_->size().width, instance_->size().height));
        create<false, cv::Size>(Keys::WINDOW_SIZE, instance_->size(), [](const cv::Size& sz) { CV_UNUSED(sz); });
        create<true>(Keys::FRAMEBUFFER_SIZE, instance_->size());
        create<false>(Keys::CLEAR_COLOR, cv::Scalar(0, 0, 0, 255));
        create<false, string>(Keys::NAMESPACE, "default");
        create<false, bool>(Keys::FULLSCREEN, false, [](const bool& fs) { CV_UNUSED(fs); });
        create<false>(Keys::DISABLE_INPUT_EVENTS, false);
        create<false, bool>(Keys::VISIBLE, true, [](const bool& v) { CV_UNUSED(v); });
    }

    template<bool Tread, typename Tval>
    static void create(Keys::Enum key, const Tval& val,
                       const std::function<void(const Tval& val)>& cb = std::function<void(const Tval& val)>()) {
        properties_.create<Tread>(key, val, cb);
    }

    template<typename Tval>
    static void set(Keys::Enum key, const Tval& val, bool fire = true) {
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

    CV_EXPORTS std::string title() const;
    CV_EXPORTS const cv::Size& size();

    CV_EXPORTS cv::Ptr<detail::FrameBufferContext> fbCtx();
    CV_EXPORTS cv::Ptr<detail::SourceContext> sourceCtx();
    CV_EXPORTS cv::Ptr<detail::SinkContext> sinkCtx();
    CV_EXPORTS cv::Ptr<detail::PlainContext> plainCtx();

    CV_EXPORTS cv::Ptr<detail::PlanContext> context(const string& name, int32_t idx = -1);
    CV_EXPORTS void registerContext(const string& name, cv::Ptr<detail::PlanContext> ctx, int32_t idx = -1);

    CV_EXPORTS void setSource(cv::Ptr<Source> src);
    CV_EXPORTS cv::Ptr<Source> getSource();
    CV_EXPORTS bool hasSource() const;
    CV_EXPORTS void setSink(cv::Ptr<Sink> sink);
    CV_EXPORTS cv::Ptr<Sink> getSink();
    CV_EXPORTS bool hasSink() const;

    CV_EXPORTS void setDisplayCallback(std::function<bool()> cb);
    CV_EXPORTS bool display();

    CV_EXPORTS static void run(cv::Ptr<Runtime> runtime, std::function<void()> runGraph);
};

class CV_EXPORTS Plan {
    friend class Runtime;
    friend class SharedVariables;

    struct BranchState {
        string branchID_;
        bool isEnabled_ = true;
        bool isOnce_ = false;
        bool isSingle_ = false;
        bool condition_ = false;
        bool isLocked_ = false;
    };

    cv::Ptr<Runtime> runtime_ = Runtime::instance();
    std::string parent_;
    cv::UMat captureFrame_;
    cv::UMat writerFrame_;
    size_t parentOffset_ = 0;
    size_t parentActualTypeSize_ = 0;
    size_t actualTypeSize_ = 0;
    cv::Ptr<Plan> self_;
    std::vector<std::tuple<std::string, bool, size_t>> accesses_;
    std::map<std::string, cv::Ptr<Transaction>> transactions_;
    std::vector<cv::Ptr<Node>> currentNodes_;
    std::vector<cv::Ptr<Node>> allNodes_;
    std::deque<BranchState> branchStateStack_;
    std::deque<std::pair<string, BranchType::Enum>> branchStack_;

    template<typename Tedge>
    void emit_access(const string& context, Tedge tp) {
        accesses_.push_back(std::make_tuple(context, Tedge::read_t::value, tp.id()));
    }

    template<typename Tfn, typename ...Args>
    void add_transaction(std::function<cv::Ptr<PlanContext>()> ctxCb, string txID, Tfn fn, Args ...args) {
        auto tx = make_transaction(fn, args...);
        tx->setContextCallback(ctxCb);
        tx->setBranchType(BranchType::NONE);
        transactions_.insert({ txID, tx });
    }

    template<typename Tfn, typename ...Args>
    void add_transaction(BranchType::Enum btype, std::function<cv::Ptr<PlanContext>()> ctxCb, string txID, Tfn fn, Args ...args) {
        auto tx = make_transaction(fn, args...);
        tx->setContextCallback(ctxCb);
        tx->setBranchType(btype);
        transactions_.insert({ txID, tx });
    }

    template<typename Tfn, typename ...Args>
    void add_transaction(cv::Ptr<PlanContext> ctx, const string& txID, Tfn fn, Args ...args) {
        this->add_transaction([ctx]() { return ctx; }, txID, fn, args...);
    }

    template<typename Tfn, typename ...Args>
    void add_transaction(BranchType::Enum btype, cv::Ptr<PlanContext> ctx, const string& txID, Tfn fn, Args ...args) {
        this->add_transaction(btype, [ctx]() { return ctx; }, txID, fn, args...);
    }

    template <typename ... Args, typename Tfn>
    static auto wrap_callable(Tfn fn) {
        if constexpr (std::is_void<typename CallableTraits<Tfn>::return_type_t>::value ||
                      std::is_same<typename CallableTraits<Tfn>::return_type_t, std::false_type>::value) {
            if constexpr (CallableTraits<Tfn>::member_t::value) {
                return std::function([fn](Args... values) -> decltype(std::mem_fn(fn)(values...)) {
                    return std::mem_fn(fn)(values...);
                });
            } else {
                return std::function(fn);
            }
        } else {
            if constexpr (CallableTraits<Tfn>::member_t::value) {
                return std::function([fn](Args... values) -> decltype(std::mem_fn(fn)(values...)) {
                    return std::mem_fn(fn)(values...);
                });
            } else {
                return std::function(fn);
            }
        }
    }

    template<bool Tconst, typename T>
    auto makeInternalEdge(T& val) {
        if constexpr (Tconst) {
            return R(val);
        } else {
            return RW(val);
        }
    }

    template<typename T>
    void setActualTypeSize() { actualTypeSize_ = sizeof(T); }
    template<typename T>
    void setParentActualTypeSize() { parentActualTypeSize_ = sizeof(T); }
    void setParentOffset(size_t offset) { parentOffset_ = offset; }
    size_t getActualTypeSize() { return actualTypeSize_; }
    size_t getParentActualTypeSize() { return parentActualTypeSize_; }
    size_t getParentOffset() { return parentOffset_; }

    void findNode(const string& name, cv::Ptr<Node>& found) {
        CV_Assert(!name.empty());
        if (currentNodes_.empty())
            return;
        if (currentNodes_.back()->name_ == name)
            found = currentNodes_.back();
    }

    void makeGraph() {
        for (const auto& t : accesses_) {
            const string& name = std::get<0>(t);
            const bool& read = std::get<1>(t);
            const size_t& dep = std::get<2>(t);
            cv::Ptr<Node> n;
            findNode(name, n);
            if (!n) {
                n = new Node();
                n->name_ = name;
                n->tx_ = transactions_[name];
                CV_Assert(!n->name_.empty());
                CV_Assert(n->tx_);
                currentNodes_.push_back(n);
            }
            if (read) {
                n->read_deps_.insert(dep);
            } else {
                n->write_deps_.insert(dep);
            }
        }
    }

    void runGraph() {
        BranchType::Enum btype;
        BranchState currentState;
        try {
            for (auto& n : currentNodes_) {
                btype = n->tx_->getBranchType();
                bool isBranch = n->name_.substr(0, 6) == "branch";
                bool isElse = n->name_.substr(0, 6) == "[else]";
                bool isEnd = n->name_.substr(0, 5) == "[end]";

                if (btype != BranchType::NONE) {
                    CV_Assert(((isBranch != isElse) != isEnd));
                    if (isBranch) {
                        if (!branchStateStack_.empty())
                            currentState = branchStateStack_.front();
                        else
                            currentState = BranchState();
                        currentState.branchID_ = n->name_;
                        if (currentState.isEnabled_) {
                            currentState.isOnce_ = ((btype == BranchType::ONCE) || (btype == BranchType::PARALLEL_ONCE));
                            currentState.isSingle_ = ((btype == BranchType::ONCE) || (btype == BranchType::SINGLE));
                        } else {
                            currentState.isOnce_ = false;
                            currentState.isSingle_ = false;
                            currentState.isEnabled_ = false;
                        }
                        if (currentState.isEnabled_) {
                            if (currentState.isOnce_) {
                                if ((btype == BranchType::ONCE)) {
                                    currentState.condition_ = GlobalState::once(n->name_) && n->tx_->performPredicate();
                                } else if ((btype == BranchType::PARALLEL_ONCE)) {
                                    currentState.condition_ = !n->tx_->ran() && n->tx_->performPredicate();
                                } else {
                                    CV_Assert(false);
                                }
                            } else {
                                currentState.condition_ = n->tx_->performPredicate();
                            }
                            currentState.isEnabled_ = currentState.isEnabled_ && currentState.condition_;
                            if (currentState.isEnabled_ && currentState.isSingle_) {
                                CV_Assert(btype != BranchType::PARALLEL);
                                GlobalState::lockNode(currentState.branchID_);
                                currentState.isLocked_ = true;
                            }
                        }
                        branchStateStack_.push_front(currentState);
                    } else if (isElse) {
                        if (branchStateStack_.empty())
                            continue;
                        currentState = branchStateStack_.front();
                        currentState.isEnabled_ = !currentState.condition_;
                        currentState.isOnce_ = false;
                        currentState.condition_ = !currentState.condition_;
                        currentState.isSingle_ = false;
                        if (currentState.isLocked_) {
                            GlobalState::tryUnlockNode(currentState.branchID_);
                        }
                        currentState.isLocked_ = false;
                        branchStateStack_.pop_front();
                        branchStateStack_.push_front(currentState);
                    } else if (isEnd) {
                        if (branchStateStack_.empty())
                            continue;
                        currentState = branchStateStack_.front();
                        GlobalState::tryUnlockNode(currentState.branchID_);
                        branchStateStack_.pop_front();
                    } else {
                        CV_Assert(false);
                    }
                } else {
                    CV_Assert(!n->tx_->isPredicate());
                    currentState = !branchStateStack_.empty() ? branchStateStack_.front() : BranchState();
                    if (currentState.isEnabled_) {
                        auto lock = GlobalState::tryGetNodeLock(currentState.branchID_);
                        auto plan = self<Plan>();
                        auto ctx = n->tx_->getContextCallback()();
                        auto viewport = Runtime::get<cv::Rect>(Runtime::Keys::VIEWPORT);
                        if (lock) {
                            std::lock_guard<std::mutex> guard(*lock.get());
                            int res = ctx->execute(viewport, [plan, n]() {
                                TimeTracker::getInstance()->execute(n->name_, [n]() { n->tx_->perform(); });
                            });
                            if (res <= 0) {
                                CV_LOG_WARNING(nullptr, "Context failed while: " + n->name_);
                            }
                        } else {
                            int res = ctx->execute(viewport, [plan, n]() {
                                TimeTracker::getInstance()->execute(n->name_, [n]() { n->tx_->perform(); });
                            });
                            if (res <= 0) {
                                CV_LOG_WARNING(nullptr, "Context failed while: " + n->name_);
                            }
                        }
                    }
                    currentState = BranchState();
                }
            }
            size_t lockCnt = GlobalState::countNodeLocks();
            CV_Assert(branchStateStack_.empty());
            CV_Assert(lockCnt == 0);
        } catch (std::exception& ex) {
            if (!branchStateStack_.empty() && branchStateStack_.front().isLocked_) {
                GlobalState::tryUnlockNode(currentState.branchID_);
            }
            throw;
        } catch (...) {
            if (!branchStateStack_.empty() && branchStateStack_.front().isLocked_) {
                GlobalState::tryUnlockNode(currentState.branchID_);
            }
            throw std::runtime_error("Unknown error.");
        }
    }

    void clearGraph() {
        std::copy(currentNodes_.begin(), currentNodes_.end(), std::back_inserter(allNodes_));
        accesses_.clear();
        branchStateStack_.clear();
        branchStack_.clear();
        transactions_.clear();
        currentNodes_.clear();
    }

    template<typename Tinstance>
    cv::Ptr<Tinstance> self() {
        if (!self_)
            self_ = this;
        return self_.dynamicCast<Tinstance>();
    }

    template<typename Tplan, typename Tparent, typename ... Args>
    static cv::Ptr<Tplan> makeSubPlan(Tparent* parent, Args&& ... args) {
        Tplan* plan = new Tplan(std::forward<Args>(args)...);
        plan->setParentID(parent->space());
        plan->setParentOffset(reinterpret_cast<size_t>(parent));
        plan->template setParentActualTypeSize<Tparent>();
        plan->template setActualTypeSize<Tplan>();
        Runtime::set(Runtime::Keys::NAMESPACE, plan->space());
        return plan->template self<Tplan>();
    }

    template<typename Tfn, typename ... Args>
    const string make_id(string id, const string& name, Tfn fn, Args ... args) {
        stringstream ss;
        if (!id.empty())
            id = "::" + id;
        if constexpr (std::is_pointer<Tfn>::value) {
            ss << name << id << " [" << detail::int_to_hex(reinterpret_cast<size_t>(fn)) << "] ";
        } else {
            ss << name << id << " [" << detail::lambda_ptr_hex(std::forward<Tfn>(fn)) << "] ";
        }
        ((ss << detail::demangle(typeid(typename std::remove_reference_t<decltype(args)>::ref_t).name())
             << "(" << detail::int_to_hex(args.id()) << ") "), ...);
        ss << "- " << detail::map_index(std::this_thread::get_id());
        while (transactions_.find(ss.str()) != transactions_.end()) {
            ss << '+';
        }
        return ss.str();
    }

    template <typename Tctx, typename Tfn, typename Tuple, size_t ... idx>
    cv::Ptr<Plan> call(Tctx ctx, const string& name, Tfn fn, Tuple&& args, std::index_sequence<idx...>) {
        const string id = make_id(this->space(), name, fn, std::get<idx>(args)...);
        emit_access(id, R(*this));
        (emit_access(id, std::get<idx>(args)), ...);
        auto wrap = wrap_callable<typename std::remove_reference<decltype(std::get<idx>(args))>::type::ref_t...>(fn);
        add_transaction(ctx, id, wrap, std::get<idx>(args)...);
        return self<Plan>();
    }

public:
    template<typename T>
    struct Property : detail::Edge<const T, false, true, true> {
        using parent_t = detail::Edge<const T, false, true, true>;
        Property(cv::Ptr<Plan> plan, const T& val) : parent_t(parent_t::make(plan, val)) {
            GlobalState::shared_vars().makeSharedVar(val);
        }
    };

    template<typename TeventClass,
             typename Tfn = std::function<std::vector<std::shared_ptr<TeventClass>>()>,
             typename Tparent = detail::Edge<Tfn, false, true, false>>
    struct Event : Tparent {
        Event(cv::Ptr<Plan> plan) : Tparent(Tparent::make(plan, wrap_callable<>([]() {
            if (!Runtime::instance()->get<bool>(Runtime::Keys::DISABLE_INPUT_EVENTS))
                return event::fetch<TeventClass>();
            else
                return typename TeventClass::List();
        }))) {
            static_assert(Tparent::func_t::value, "Internal error: Function not recognized!");
        }
        Event(cv::Ptr<Plan> plan, const typename TeventClass::Type t) : Tparent(Tparent::make(plan, wrap_callable<>([t]() {
            if (!Runtime::instance()->get<bool>(Runtime::Keys::DISABLE_INPUT_EVENTS))
                return event::fetch<TeventClass>(t);
            else
                return typename TeventClass::List();
        }))) {
            static_assert(Tparent::func_t::value, "Internal error: Function not recognized!");
        }
        template<typename Ttrigger>
        Event(cv::Ptr<Plan> plan, const typename TeventClass::Type t, const Ttrigger tr)
            : Tparent(Tparent::make(plan, wrap_callable<>([t, tr]() {
                if (!Runtime::instance()->get<bool>(Runtime::Keys::DISABLE_INPUT_EVENTS))
                    return event::fetch<TeventClass>(t, tr);
                else
                    return typename TeventClass::List();
            }))) {
            static_assert(Tparent::func_t::value, "Internal error: Function not recognized!");
        }
    };

    // predefined branch predicates
    constexpr static auto always_ = []() { return true; };
    constexpr static auto isTrue_ = [](const bool& b) { return b; };
    constexpr static auto isFalse_ = [](const bool& b) { return !b; };
    constexpr static auto and_ = [](const bool& a, const bool& b) { return a && b; };
    constexpr static auto or_ = [](const bool& a, const bool& b) { return a || b; };

    virtual ~Plan() { self_ = nullptr; }
    virtual void gui() {}
    virtual void setup() {}
    virtual void infer() = 0;
    virtual void teardown() {}

    virtual std::string space() {
        if (!parent_.empty()) {
            return parent_ + "-" + name();
        } else
            return name();
    }
    virtual std::string name() {
        return detail::demangle(typeid(*this).name());
    }
    virtual void setParentID(const string& parent) { parent_ = parent; }
    virtual std::string getParentID() { return parent_; }

    // ---- Generic named-context calls (replace the old gl/ext/nvg/bgfx) ----
    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of<EdgeBase, Tfn>::value, cv::Ptr<Plan>>::type
    gl(Tfn fn, Args ... args) {
        auto argsTuple = std::make_tuple(args...);
        call(runtime_->context("gl", -1), "gl", fn, std::forward<decltype(argsTuple)>(argsTuple),
             std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
        return self<Plan>();
    }

    template <int32_t pos = 0, typename Tedge, typename Tfn, typename ... Args>
    typename std::enable_if<std::is_base_of<EdgeBase, Tedge>::value, cv::Ptr<Plan>>::type
    gl(Tedge indexEdge, Tfn fn, Args ... args) {
        auto ctxCallback = [this, indexEdge]() {
            Tedge copy = indexEdge;
            return runtime_->context("gl", copy.ref());
        };
        auto argsTuple = std::make_tuple(args...);
        if constexpr (pos > 0) {
            auto beforePos = detail::sub_tuple<0, pos>(argsTuple);
            auto afterPos = detail::sub_tuple<pos, sizeof...(args) - pos>(argsTuple);
            auto allTuple = std::tuple_cat(beforePos, std::make_tuple(indexEdge), afterPos);
            return call(ctxCallback, "gl-i", fn, std::forward<decltype(allTuple)>(allTuple),
                        std::make_index_sequence<std::tuple_size<decltype(allTuple)>::value>());
        } else if constexpr (pos < 0) {
            return call(ctxCallback, "gl-i", fn, std::forward<decltype(argsTuple)>(argsTuple),
                        std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
        } else {
            auto allTuple = std::make_tuple(indexEdge, args...);
            return call(ctxCallback, "gl-i", fn, std::forward<decltype(allTuple)>(allTuple),
                        std::make_index_sequence<std::tuple_size<decltype(allTuple)>::value>());
        }
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of<EdgeBase, Tfn>::value, cv::Ptr<Plan>>::type
    ext(Tfn fn, Args ... args) {
        auto argsTuple = std::make_tuple(args...);
        call(runtime_->context("ext", -1), "ext", fn, std::forward<decltype(argsTuple)>(argsTuple),
             std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
        return self<Plan>();
    }

    template <int32_t pos = 0, typename Tedge, typename Tfn, typename ... Args>
    typename std::enable_if<std::is_base_of<EdgeBase, Tedge>::value, cv::Ptr<Plan>>::type
    ext(Tedge indexEdge, Tfn fn, Args ... args) {
        auto ctxCallback = [this, indexEdge]() {
            Tedge copy = indexEdge;
            return runtime_->context("ext", copy.ref());
        };
        auto argsTuple = std::make_tuple(args...);
        if constexpr (pos > 0) {
            auto beforePos = detail::sub_tuple<0, pos>(argsTuple);
            auto afterPos = detail::sub_tuple<pos, sizeof...(args) - pos>(argsTuple);
            auto allTuple = std::tuple_cat(beforePos, std::make_tuple(indexEdge), afterPos);
            return call(ctxCallback, "ext-i", fn, std::forward<decltype(allTuple)>(allTuple),
                        std::make_index_sequence<std::tuple_size<decltype(allTuple)>::value>());
        } else if constexpr (pos < 0) {
            return call(ctxCallback, "ext-i", fn, std::forward<decltype(argsTuple)>(argsTuple),
                        std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
        } else {
            auto allTuple = std::make_tuple(indexEdge, args...);
            return call(ctxCallback, "ext-i", fn, std::forward<decltype(allTuple)>(allTuple),
                        std::make_index_sequence<std::tuple_size<decltype(allTuple)>::value>());
        }
    }

    template <typename Tfn, typename ... Args>
    cv::Ptr<Plan> nvg(Tfn fn, Args... args) {
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "nvg", fn, args...);
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        add_transaction(runtime_->context("nvg", -1), id, wrap, args...);
        return self<Plan>();
    }

    template <typename Tfn, typename ... Args>
    cv::Ptr<Plan> bgfx(Tfn fn, Args... args) {
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "bgfx", fn, args...);
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        add_transaction(runtime_->context("bgfx", -1), id, wrap, args...);
        return self<Plan>();
    }

    // ---- Branching ----
    template <typename Tedge>
    typename std::enable_if<std::is_base_of_v<EdgeBase, Tedge>, cv::Ptr<Plan>>::type
    branch(Tedge edge) {
        auto wrap = wrap_callable<typename Tedge::ref_t>([](const bool& b) { return b; });
        const string id = make_id(this->space(), "branch", wrap);
        branchStack_.push_front({ id, BranchType::PARALLEL });
        emit_access(id, R(*this));
        add_transaction(BranchType::PARALLEL, runtime_->plainCtx(), id, wrap, edge);
        return self<Plan>();
    }

    template <typename Tfn>
    typename std::enable_if<!std::is_base_of_v<EdgeBase, Tfn>, cv::Ptr<Plan>>::type
    branch(Tfn fn) {
        auto wrap = wrap_callable(fn);
        const string id = make_id(this->space(), "branch", fn);
        branchStack_.push_front({ id, BranchType::PARALLEL });
        emit_access(id, R(*this));
        add_transaction(BranchType::PARALLEL, runtime_->plainCtx(), id, wrap);
        return self<Plan>();
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_integral_v<Tfn>, cv::Ptr<Plan>>::type
    branch(Tfn fn, Args ... args) {
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "branch", fn, args...);
        branchStack_.push_front({ id, BranchType::PARALLEL });
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        add_transaction(BranchType::PARALLEL, runtime_->plainCtx(), id, wrap, args...);
        return self<Plan>();
    }

    template <typename Tedge>
    typename std::enable_if<std::is_base_of_v<EdgeBase, Tedge>, cv::Ptr<Plan>>::type
    branch(BranchType::Enum type, Tedge edge) {
        auto wrap = wrap_callable<typename Tedge::ref_t>([](const bool& b) { return b; });
        const string id = make_id(this->space(), "branch", wrap);
        branchStack_.push_front({ id, type });
        emit_access(id, R(*this));
        add_transaction(type, runtime_->plainCtx(), id, wrap, edge);
        return self<Plan>();
    }

    template <typename Tfn, typename ... Args>
    cv::Ptr<Plan> branch(BranchType::Enum type, Tfn fn, Args ... args) {
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "branch-type" + std::to_string((int)type), fn, args...);
        branchStack_.push_front({ id, type });
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        add_transaction(type, runtime_->plainCtx(), id, wrap, args...);
        return self<Plan>();
    }

    cv::Ptr<Plan> endBranch() {
        auto current = branchStack_.front();
        branchStack_.pop_front();
        string id = "[end]" + current.first;
        emit_access(id, R(*this));
        std::function<bool()> functor = []() { return true; };
        add_transaction(current.second, runtime_->plainCtx(), id, functor);
        return self<Plan>();
    }

    cv::Ptr<Plan> elseBranch() {
        auto current = branchStack_.front();
        string id = "[else]" + current.first;
        emit_access(id, R(*this));
        std::function<bool()> functor = []() { return true; };
        add_transaction(current.second, runtime_->plainCtx(), id, functor);
        return self<Plan>();
    }

    // ---- Plain context ----
    template <typename ... Args>
    cv::Ptr<Plan> plain(Args... args) {
        auto fn = [](typename Args::ref_t ...) {};
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "plain", wrap, args...);
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        add_transaction(runtime_->plainCtx(), id, wrap, args...);
        return self<Plan>();
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of<EdgeBase, Tfn>::value, cv::Ptr<Plan>>::type
    plain(Tfn fn, Args... args) {
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "plain", fn, args...);
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        add_transaction(runtime_->plainCtx(), id, wrap, args...);
        return self<Plan>();
    }

    // ---- Framebuffer context ----
    template <size_t pos = 0, typename Tfn, typename ... Args>
    cv::Ptr<Plan> fb(Tfn fn, Args ... args) {
        using isMemFn = typename CallableTraits<Tfn>::member_t;
        constexpr size_t idx = pos > 0 && isMemFn::value ? pos - 1 : pos;
        using Tfb = typename std::tuple_element<idx, typename function_traits<Tfn>::argument_types>::type;
        auto argsTuple = std::make_tuple(args...);
        if constexpr (pos > 0) {
            auto beforeFb = detail::sub_tuple<0, pos>(argsTuple);
            auto afterFb = detail::sub_tuple<pos, sizeof...(args) - pos>(argsTuple);
            auto fbEdge = std::make_tuple(makeInternalEdge<std::is_const<Tfb>::value>(runtime_->fbCtx()->fb()));
            auto allTuple = std::tuple_cat(beforeFb, fbEdge, afterFb);
            return call(runtime_->fbCtx(), "fb", fn, std::forward<decltype(allTuple)>(allTuple),
                        std::make_index_sequence<std::tuple_size<decltype(allTuple)>::value>());
        } else {
            auto allTuple = std::make_tuple(makeInternalEdge<std::is_const<Tfb>::value>(runtime_->fbCtx()->fb()), args...);
            return call(runtime_->fbCtx(), "fb", fn, std::forward<decltype(allTuple)>(allTuple),
                        std::make_index_sequence<std::tuple_size<decltype(allTuple)>::value>());
        }
    }

    cv::Ptr<Plan> clear() {
        return fb([](cv::UMat& framebuffer, const cv::Scalar& cc) { framebuffer.setTo(cc); },
                  P<cv::Scalar>(Runtime::Keys::CLEAR_COLOR));
    }

    // ---- Source / Sink ----
    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of_v<EdgeBase, Tfn>, cv::Ptr<Plan>>::type
    capture(Tfn fn, Args ... args) {
        auto srcEdge = makeInternalEdge<false>(runtime_->sourceCtx()->sourceBuffer());
        auto argsTuple = std::make_tuple(srcEdge, args...);
        return call(runtime_->sourceCtx(), "src", fn, std::forward<decltype(argsTuple)>(argsTuple),
                    std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
    }

    template <typename Tedge>
    cv::Ptr<Plan> capture(Tedge&& edge) {
        capture([](const cv::UMat& inputFrame, cv::UMat& f) {
            if (!inputFrame.empty())
                inputFrame.copyTo(f);
        }, edge);
        return self<Plan>();
    }

    cv::Ptr<Plan> capture() {
        capture([](const cv::UMat& inputFrame, cv::UMat& f) {
            if (!inputFrame.empty())
                inputFrame.copyTo(f);
        }, detail::Edge<cv::UMat, false, false>::make(self<Plan>(), captureFrame_));
        fb([](cv::UMat& framebuffer, const cv::UMat& f) {
            if (!f.empty()) {
                cv::resize(f, framebuffer, framebuffer.size(), INTER_LINEAR);
            }
        }, detail::Edge<cv::UMat, false, true>::make(self<Plan>(), captureFrame_));
        return self<Plan>();
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of_v<EdgeBase, Tfn>, cv::Ptr<Plan>>::type
    write(Tfn fn, Args ... args) {
        if (!getParentID().empty())
            return self<Plan>();
        using Tfb = typename std::tuple_element<0, typename function_traits<Tfn>::argument_types>::type;
        static_assert((std::is_same<Tfb, cv::UMat>::value) || !"The first argument must be of type 'cv::UMat&'");
        auto sinkEdge = makeInternalEdge<std::is_const<Tfb>::value>(runtime_->sinkCtx()->sinkBuffer());
        auto argsTuple = std::make_tuple(sinkEdge, args...);
        return call(runtime_->sinkCtx(), "sink", fn, std::forward<decltype(argsTuple)>(argsTuple),
                    std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
    }

    template<typename Tedge>
    cv::Ptr<Plan> write(Tedge&& edge) {
        if (!getParentID().empty())
            return self<Plan>();
        write([](cv::UMat& outputFrame, const cv::UMat& f) {
            f.copyTo(outputFrame);
        }, edge);
        return self<Plan>();
    }

    cv::Ptr<Plan> write() {
        if (!getParentID().empty())
            return self<Plan>();
        auto writerEdge = makeInternalEdge<false>(writerFrame_);
        auto writerEdgeConst = makeInternalEdge<true>(writerFrame_);
        fb([](const cv::UMat& framebuffer, cv::UMat& f) {
            framebuffer.copyTo(f);
        }, writerEdge);
        write([](cv::UMat& outputFrame, const cv::UMat& f) {
            f.copyTo(outputFrame);
        }, writerEdgeConst);
        return self<Plan>();
    }

    // ---- Sub-plans ----
    template <typename TsubPlan>
    cv::Ptr<Plan> subInfer(cv::Ptr<TsubPlan> subPlan) {
        subPlan->infer();
        subPlan->makeGraph();
        std::copy(subPlan->accesses_.begin(), subPlan->accesses_.end(), std::inserter(accesses_, accesses_.end()));
        std::copy(subPlan->transactions_.begin(), subPlan->transactions_.end(),
                  std::inserter(transactions_, transactions_.end()));
        subPlan->clearGraph();
        return self<Plan>();
    }

    template <typename TsubPlan>
    cv::Ptr<Plan> subSetup(cv::Ptr<TsubPlan> subPlan) {
        subPlan->setup();
        subPlan->makeGraph();
        std::copy(subPlan->accesses_.begin(), subPlan->accesses_.end(), std::inserter(accesses_, accesses_.end()));
        std::copy(subPlan->transactions_.begin(), subPlan->transactions_.end(),
                  std::inserter(transactions_, transactions_.end()));
        subPlan->clearGraph();
        return self<Plan>();
    }

    template <typename TsubPlan>
    cv::Ptr<Plan> subTeardown(cv::Ptr<TsubPlan> subPlan) {
        subPlan->teardown();
        subPlan->makeGraph();
        std::copy(subPlan->accesses_.begin(), subPlan->accesses_.end(), std::inserter(accesses_, accesses_.end()));
        std::copy(subPlan->transactions_.begin(), subPlan->transactions_.end(),
                  std::inserter(transactions_, transactions_.end()));
        subPlan->clearGraph();
        return self<Plan>();
    }

    // ---- set ----
    template <typename Tkey, typename Tfn, typename Ttuple, size_t ... idx>
    auto make_setter_function(Tkey key, Tfn fn, Ttuple&& args, std::index_sequence<idx...>) {
        auto plan = self<Plan>();
        return std::function([plan, key, fn](decltype(std::get<idx>(args).ref()) ... values) {
            Runtime::set(key, fn(values...));
        });
    }

    template <typename Ttuple>
    auto make_setter_function(Ttuple&& values) {
        using tuple_t = typename std::remove_reference<Ttuple>::type;
        constexpr size_t sz = std::tuple_size<tuple_t>::value;
        static_assert(std::is_enum<typename std::tuple_element<0, tuple_t>::type>::value,
                      "Can not set a property without a key as first argument");
        static_assert(sz > 1, "Can not set a property without value");
        auto key = std::get<0>(values);
        auto val = std::get<1>(values);
        if constexpr (!is_callable<decltype(val)>::value) {
            static_assert(sz == 2, "Can not set a Property from multiple Edges");
            auto plan = self<Plan>();
            return std::function([plan, key](decltype(val.ref()) v) {
                Runtime::set(key, v);
            });
        } else {
            auto fn = std::get<1>(values);
            auto args = detail::sub_tuple<1, sz - 1>(values);
            auto plan = self<Plan>();
            return make_setter_function(key, fn, args, std::make_index_sequence<sz - 2>());
        }
    }

    template <typename TwrapFn, typename Ttuple, size_t ... idx>
    cv::Ptr<Plan> set(const string& id, TwrapFn fn, Ttuple&& args, std::index_sequence<idx...>) {
        auto wrap = wrap_callable<decltype(std::get<0>(args))>(fn);
        emit_access(id, R(*this));
        (emit_access(id, std::get<idx>(args)), ...);
        add_transaction(runtime_->plainCtx(), id, wrap, std::get<idx>(args)...);
        return self<Plan>();
    }

    template <typename Tedge, size_t ... idx>
    cv::Ptr<Plan> set(std::tuple<Runtime::Keys::Enum, Tedge>&& values, std::index_sequence<idx...>) {
        const string id = make_id(this->space(), "set-fn", std::get<idx>(values)...);
        std::function wrap = make_setter_function(values);
        auto args = detail::sub_tuple<1, std::tuple_size<std::tuple<Runtime::Keys::Enum, Tedge>>::value - 1>(values);
        return set(id, wrap, std::forward<decltype(args)>(args),
                   std::make_index_sequence<std::tuple_size<decltype(args)>::value>());
    }

    template <typename Tedge>
    cv::Ptr<Plan> set(std::tuple<Runtime::Keys::Enum, Tedge>&& values) {
        using sz = std::tuple_size<std::tuple<Runtime::Keys::Enum, Tedge>>;
        return set(std::forward<std::tuple<Runtime::Keys::Enum, Tedge>>(values), std::make_index_sequence<sz::value>());
    }

    template<typename Tedge>
    cv::Ptr<Plan> set(const Runtime::Keys::Enum& key, const Tedge& e) {
        return set(std::make_tuple(key, e), std::make_index_sequence<1>());
    }

    template<typename ... Args>
    cv::Ptr<Plan> set(std::tuple<Runtime::Keys::Enum, Args>&& ... tuples) {
        (set(std::forward<std::tuple<Runtime::Keys::Enum, Args>>(tuples)), ...);
        return self<Plan>();
    }

    // ---- Operators ----
    template<bool TmakeEdge = true, typename TfnOp, typename ... Args>
    auto make_op(TfnOp fn, Args ... args) {
        auto op = wrap_callable<typename Args::ref_t ...>(fn);
        using ret_t = typename CallableTraits<decltype(op)>::return_type_t;
        constexpr bool hasReturn = !std::is_same<ret_t, void>::value;
        using ret_no_ref_t = typename std::remove_reference<ret_t>::type;
        static_assert(!std::is_same<ret_no_ref_t, std::false_type>::value, "Invalid callable passed to Plan::op");
        using val_t = typename std::disjunction<
            values_equal<hasReturn, true, typename std::remove_pointer<ret_no_ref_t>::type>,
            default_type<int>
        >::type;

        if constexpr (hasReturn && TmakeEdge) {
            cv::Ptr<cv::Ptr<val_t>> retPtr = new cv::Ptr<val_t>(cv::Ptr<val_t>(), nullptr);
            std::function wrap = [op](cv::Ptr<val_t>& v, typename Args::ref_t ... values) mutable {
                if constexpr (std::is_pointer<ret_no_ref_t>::value) {
                    v = cv::Ptr<val_t>(cv::Ptr<val_t>(), op(values...));
                } else if constexpr (std::is_lvalue_reference<ret_t>::value) {
                    auto& ref = op(values...);
                    v = cv::Ptr<val_t>(cv::Ptr<val_t>(), std::addressof(ref));
                } else {
                    v = cv::Ptr<val_t>(cv::Ptr<val_t>(), new val_t(op(values...)));
                }
            };
            const string id = make_id(this->space(), "nary-op", wrap, args...);
            emit_access(id, R(*this));
            (emit_access(id, args), ...);
            auto ptrEdge = detail::Edge<cv::Ptr<cv::Ptr<val_t>>, false, false, false, cv::Ptr<val_t>, true>::make(self<Plan>(), retPtr);
            add_transaction(runtime_->plainCtx(), id, wrap, ptrEdge, args...);
            return detail::Edge<cv::Ptr<val_t>, false, false, false, val_t>::make(self<Plan>(), *retPtr.get());
        } else {
            std::function wrap = [op](typename Args::ref_t ... values) {
                op(values...);
            };
            const string id = make_id(this->space(), "nary-op", wrap, args...);
            emit_access(id, R(*this));
            (emit_access(id, args), ...);
            add_transaction(runtime_->plainCtx(), id, wrap, args...);
            return self<Plan>();
        }
    }

    template<Operators Top, typename ... Edges>
    cv::Ptr<Plan> op(Edges ... edges) {
        return make_op<false>(make_operator_func<check_op<Top, Edges...>::value>(edges...), edges...);
    }

    template<typename ... Edges>
    cv::Ptr<Plan> assign(Edges ... edges) {
        return make_op<false>(make_operator_func<check_op<ASSIGN_, Edges...>::value>(edges...), edges...);
    }

    template<typename ... Edges>
    cv::Ptr<Plan> construct(Edges ... edges) {
        return make_op<false>(make_operator_func<check_op<CONSTRUCT_, Edges...>::value>(edges...), edges...);
    }

    template<Operators Top, typename ... Edges>
    auto OP(Edges ... edges) {
        return make_op(make_operator_func<check_op<Top, Edges...>::value>(edges...), edges...);
    }

    template<typename ... Edges>
    auto operator()(Edges&& ... edges) { return OP<Operators::CONSTRUCT_>(edges...); }
    template<typename ... Edges>
    auto IF(Edges&& ... edges) { return OP<Operators::IF_>(edges...); }
    template<typename ... Edges>
    auto ASSIGN(Edges&& ... edges) { return OP<Operators::ASSIGN_>(edges...); }
    template<typename ... Edges>
    auto SUB(Edges&& ... edges) { return OP<Operators::SUB_>(edges...); }
    template<typename ... Edges>
    auto MUL(Edges&& ... edges) { return OP<Operators::MUL_>(edges...); }
    template<typename ... Edges>
    auto DIV(Edges&& ... edges) { return OP<Operators::DIV_>(edges...); }
    template<typename ... Edges>
    auto MOD(Edges&& ... edges) { return OP<Operators::MOD_>(edges...); }
    template<typename ... Edges>
    auto INCL(Edges&& ... edges) { return OP<Operators::INCL_>(edges...); }
    template<typename ... Edges>
    auto INCR(Edges&& ... edges) { return OP<Operators::INCR_>(edges...); }
    template<typename ... Edges>
    auto DECL(Edges&& ... edges) { return OP<Operators::DECL_>(edges...); }
    template<typename ... Edges>
    auto DECR(Edges&& ... edges) { return OP<Operators::DECR_>(edges...); }
    template<typename ... Edges>
    auto AND(Edges&& ... edges) { return OP<Operators::AND_>(edges...); }
    template<typename ... Edges>
    auto OR(Edges&& ... edges) { return OP<Operators::OR_>(edges...); }
    template<typename ... Edges>
    auto EQ(Edges&& ... edges) { return OP<Operators::EQ_>(edges...); }
    template<typename ... Edges>
    auto NEQ(Edges&& ... edges) { return OP<Operators::NEQ_>(edges...); }
    template<typename ... Edges>
    auto LT(Edges&& ... edges) { return OP<Operators::LT_>(edges...); }
    template<typename ... Edges>
    auto GT(Edges&& ... edges) { return OP<Operators::GT_>(edges...); }
    template<typename ... Edges>
    auto LE(Edges&& ... edges) { return OP<Operators::LE_>(edges...); }
    template<typename ... Edges>
    auto GE(Edges&& ... edges) { return OP<Operators::GE_>(edges...); }
    template<typename ... Edges>
    auto NOT(Edges&& ... edges) { return OP<Operators::NOT_>(edges...); }
    template<typename ... Edges>
    auto XOR(Edges&& ... edges) { return OP<Operators::XOR_>(edges...); }
    template<typename ... Edges>
    auto BAND(Edges&& ... edges) { return OP<Operators::BAND_>(edges...); }
    template<typename ... Edges>
    auto BOR(Edges&& ... edges) { return OP<Operators::BOR_>(edges...); }
    template<typename ... Edges>
    auto SHL(Edges&& ... edges) { return OP<Operators::SHL_>(edges...); }
    template<typename ... Edges>
    auto SHR(Edges&& ... edges) { return OP<Operators::SHR_>(edges...); }

    template<typename Tfn, typename ... Args>
    auto F(Tfn src, Args&& ... args) {
        return make_op(src, args...);
    }

    template<typename ... Args>
    auto _(Args&& ... args) {
        return std::make_tuple(std::forward<const Args>(args)...);
    }

    template<typename TsubPlan, typename Tparent, typename ... Args>
    auto _sub(Tparent* parent, Args&& ... args) {
        return Plan::makeSubPlan<TsubPlan>(parent, std::forward<Args>(args)...);
    }

    template<typename TsubPlan, typename TparentPtr, typename ... Args>
    auto _sub(TparentPtr parent, Args&& ... args) {
        return Plan::makeSubPlan<TsubPlan>(parent.get(), std::forward<Args>(args)...);
    }

    template<typename Tvar>
    void _shared(Tvar& val) {
        GlobalState::shared_vars().makeSharedVar(val);
    }

    template<typename Tvar>
    void _safe(Tvar& val) {
        GlobalState::shared_vars().registerSafe(val);
    }

    // ---- Edge factories ----
    template<typename T>
    detail::Edge<T, false, true> R(const T& t) {
        return detail::Edge<T, false, true>::make(self<Plan>(), t);
    }

    template<typename T>
    detail::Edge<T, false, true, true> RS(const T& t) {
        if (!GlobalState::shared_vars().checkShared(*this, t)) {
            throw std::runtime_error("You declare a non-shared variable as shared. Maybe you forgot to declare it?.");
        }
        return detail::Edge<T, false, true, true>::make(self<Plan>(), t);
    }

    template<typename T>
    detail::Edge<T, false, false> RW(T& t) {
        return detail::Edge<T, false, false>::make(self<Plan>(), t);
    }

    template<typename T>
    detail::Edge<T, false, false, true> RWS(T& t) {
        if (!GlobalState::shared_vars().checkShared(*this, t)) {
            throw std::runtime_error("You declare a non-shared variable as shared. Maybe you forgot to declare it?.");
        }
        return detail::Edge<T, false, false, true>::make(self<Plan>(), t);
    }

    template<typename T>
    detail::Edge<T, true, true, true> CS(T& t) {
        if (GlobalState::shared_vars().checkShared(*this, t)) {
            return detail::Edge<T, true, true, true>::make(self<Plan>(), t);
        } else {
            throw std::runtime_error("You are trying to safe-copy a non-shared variable. Maybe you forgot to declare it?.");
        }
    }

    template<typename T>
    detail::Edge<cv::Ptr<T>, false, true, false, T, true> V(T t) {
        auto ptr = cv::makePtr<T>(t);
        return detail::Edge<decltype(ptr), false, true, false, T, true>::make(self<Plan>(), ptr);
    }

    template<typename Tval>
    Property<Tval> P(Runtime::Keys::Enum key) {
        const auto& ref = Runtime::get<Tval>(key);
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
        Runtime::set(Runtime::Keys::NAMESPACE, plan->space());
        return plan->template self<Tplan>();
    }

    template<typename Tplan, typename ... Args>
    static void run(int32_t workers, Args&& ... args) {
        CV_Assert(workers > -2);
        if (workers == -1) {
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
            if (GlobalState::isFirstRun()) {
                GlobalState::setMainID(std::this_thread::get_id());
            }
            plan = make<Tplan>(std::forward<Args>(args)...);
            if (GlobalState::isMain()) {
                const string title = plan->runtime_->title();
                auto src = plan->runtime_->getSource();
                auto sink = plan->runtime_->getSink();
                GlobalState::set<size_t>(GlobalState::Keys::WORKERS_STARTED, workers);
                for (int32_t i = 0; i < workers; ++i) {
                    threads.push_back(
                        new std::thread([plan, i, src, sink, &args...] {
                            string name = plan->runtime_->title() + "-" + std::to_string(i);
                            setThreadName(name.c_str());
                            cv::Ptr<Runtime> worker;
                            {
                                std::lock_guard guard(worker_init_mtx_);
                                worker = Runtime::init(*plan->runtime_.get(), name);
                                if (src) {
                                    worker->setSource(src);
                                }
                                if (sink) {
                                    worker->setSink(sink);
                                }
                            }
                            LocalState::set(LocalState::Keys::WORKER_INDEX, size_t(i));
                            Plan::run<Tplan>(0, std::forward<Args>(args)...);
                        }));
                }
            }
        }
        CV_Assert(plan);

        if (GlobalState::isMain()) {
            Runtime::set(Runtime::Keys::NAMESPACE, plan->space());
            plan->gui();
        } else {
            static std::binary_semaphore setup_sema(1);
            setup_sema.acquire();
            plan->setup();
            plan->makeGraph();
            plan->runGraph();
            plan->clearGraph();
            setup_sema.release();

            plan->infer();
            plan->makeGraph();
            GlobalState::apply<size_t>(GlobalState::Keys::WORKERS_READY, [](size_t& wr) { ++wr; return wr; });
        }

        static std::barrier syncPoint(std::ptrdiff_t(workers + 1));
        syncPoint.arrive_and_wait();

        Runtime::run(plan->runtime_, [plan]() {
            TimeTracker::getInstance()->execute("iteration", [plan]() {
                plan->runGraph();
            });
        });

        if (!GlobalState::isMain()) {
            plan->clearGraph();
            plan->teardown();
            plan->makeGraph();
            plan->runGraph();
            plan->clearGraph();
            Runtime::instance()->setSink(nullptr);
            Runtime::instance()->setSource(nullptr);
        } else {
            for (auto& t : threads)
                t->join();
            Runtime::instance()->setSink(nullptr);
            Runtime::instance()->setSource(nullptr);
        }
    }
};

// ---- Free-standing operator overloads on edges ----
template<typename ... Edges>
auto operator+(const std::tuple<Edges...>& tuple) {
    return Operation::op<ADD_>(tuple);
}

template<typename TedgeL, typename ... Edges>
auto operator+(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<ADD_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator+(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator+(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename ... Edges>
auto operator-(const std::tuple<Edges...>& tuple) {
    return Operation::op<SUB_>(tuple);
}

template<typename TedgeL, typename ... Edges>
auto operator-(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<SUB_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator-(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator-(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator*(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<MUL_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator*(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator*(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator/(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<DIV_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator/(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator/(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator%(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<MOD_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator%(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator%(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename ... Edges>
auto operator++(const std::tuple<Edges...>& tuple) {
    return Operation::op<INCL_>(tuple);
}

template<typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator++(const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& e) {
    return operator++(std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(e)));
}

template<typename ... Edges>
auto operator++(const std::tuple<Edges...>& tuple, int) {
    return Operation::op<INCR_>(tuple);
}

template<typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator++(const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& e, int) {
    return operator++(std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(e)));
}

template<typename ... Edges>
auto operator--(const std::tuple<Edges...>& tuple) {
    return Operation::op<DECL_>(tuple);
}

template<typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator--(const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& e) {
    return operator--(std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(e)));
}

template<typename ... Edges>
auto operator--(const std::tuple<Edges...>& tuple, int) {
    return Operation::op<DECR_>(tuple);
}

template<typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator--(const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& e, int) {
    return operator--(std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(e)));
}

template<typename TedgeL, typename ... Edges>
auto operator&&(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<AND_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator&&(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator&&(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator||(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<OR_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator||(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator||(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator==(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<EQ_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator==(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator==(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator!=(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<NEQ_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator!=(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator!=(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator<(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<LT_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator<(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator<(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator>(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<GT_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator>(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator>(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator<=(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<LE_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator<=(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator<=(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator>=(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<GE_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator>=(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator>=(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename ... Edges>
auto operator!(const std::tuple<Edges...>& tuple) {
    return Operation::op<NOT_>(tuple);
}

template<typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator!(const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& e) {
    return operator!(std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(e)));
}

template<typename TedgeL, typename ... Edges>
auto operator^(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<XOR_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator^(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator^(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator&(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<BAND_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator&(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator&(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator|(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<BOR_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator|(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator|(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator<<(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<SHL_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator<<(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator<<(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

template<typename TedgeL, typename ... Edges>
auto operator>>(const TedgeL& lhs, const std::tuple<Edges...>& tuple) {
    return Operation::op<SHR_>(std::tuple_cat(std::make_tuple(std::forward<const TedgeL>(lhs)), tuple));
}

template<typename TedgeL, typename Telement, bool Tcopy, bool Tread, bool Tshared, typename Tbase, bool TbyValue>
auto operator>>(const TedgeL& lhs, const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>& rhs) {
    return operator>>(lhs, std::make_tuple(std::forward<const Edge<Telement, Tcopy, Tread, Tshared, Tbase, TbyValue>>(rhs)));
}

} // namespace plan
} // namespace cv

#endif // OPENCV_PLAN_PLAN_HPP_

