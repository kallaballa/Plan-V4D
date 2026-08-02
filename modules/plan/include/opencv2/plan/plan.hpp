#ifndef OPENCV_PLAN_PLAN_HPP_
#define OPENCV_PLAN_PLAN_HPP_

#include "opencv2/plan/util.hpp"
#include "opencv2/plan/context.hpp"
#include "opencv2/plan/transaction.hpp"
#include "opencv2/plan/resequence.hpp"
#include "opencv2/plan/threadsafeanymap.hpp"

#include <shared_mutex>
#include <future>
#include <set>
#include <map>
#include <string>
#include <memory>
#include <vector>
#include <barrier>
#include <type_traits>
#include <opencv2/core.hpp>
#include <opencv2/core/utils/logger.hpp>

namespace cv {
namespace plan {

using namespace cv::plan::detail;

class CV_EXPORTS Plan;

/*!
 * Runtime holds the execution contexts and properties for a Plan graph.
 * This replaces the V4D coupling with a lightweight, pluggable runtime.
 */
class CV_EXPORTS Runtime {
    friend class Plan;

public:
    struct Keys {
        enum Enum {
            SIZE,
            VIEWPORT,
            NAMESPACE
        };
    };

private:
    ThreadSafeAnyMap<Keys::Enum> properties_;
    std::map<std::string, cv::Ptr<PlanContext>> contexts_;
    cv::Ptr<PlainContext> plainContext_;
    std::mutex ctxMtx_;

public:
    CV_EXPORTS Runtime(const cv::Size& size = cv::Size(1920, 1080));
    CV_EXPORTS virtual ~Runtime();

    void init_keys(const cv::Size& size) {
        create<true>(Keys::SIZE, size);
        create<false>(Keys::VIEWPORT, cv::Rect(0, 0, size.width, size.height));
        create<false, string>(Keys::NAMESPACE, "default");
    }

    template<bool Tread, typename Tval>
    void create(Keys::Enum key, const Tval& val, const std::function<void(const Tval& val)>& cb = std::function<void(const Tval& val)>()) {
        properties_.create<Tread>(key, val, cb);
    }

    template<typename Tval>
    void set(Keys::Enum key, const Tval& val, bool fire = true) {
        properties_.set(key, val, fire);
    }

    template<typename Tval>
    const auto& get(Keys::Enum key) {
        return properties_.get<Tval>(key);
    }

    template <typename V>
    V apply(Keys::Enum k, std::function<V(V&)> f) {
        return properties_.apply(k, f);
    }

    void registerContext(const std::string& name, cv::Ptr<PlanContext> ctx) {
        std::lock_guard lock(ctxMtx_);
        contexts_[name] = ctx;
    }

    cv::Ptr<PlanContext> getContext(const std::string& name) {
        std::lock_guard lock(ctxMtx_);
        auto it = contexts_.find(name);
        if(it != contexts_.end())
            return it->second;
        return nullptr;
    }

    cv::Ptr<PlainContext> plainCtx() {
        if(!plainContext_)
            plainContext_ = new PlainContext();
        return plainContext_;
    }
};

/*!
 * Plan is a graph-based execution engine for computer vision pipelines.
 * It supports deferred execution, branching, shared state with locking,
 * sub-plan composition, and multi-worker parallelism.
 */
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

    cv::Ptr<Runtime> runtime_;
    std::string parent_;
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
        transactions_.insert({txID, tx});
    }

    template<typename Tfn, typename ...Args>
    void add_transaction(BranchType::Enum btype, std::function<cv::Ptr<PlanContext>()> ctxCb, string txID, Tfn fn, Args ...args) {
        auto tx = make_transaction(fn, args...);
        tx->setContextCallback(ctxCb);
        tx->setBranchType(btype);
        transactions_.insert({txID, tx});
    }

    template<typename Tfn, typename ...Args>
    void add_transaction(cv::Ptr<PlanContext> ctx, const string& txID, Tfn fn, Args ...args) {
        this->add_transaction([ctx](){ return ctx; }, txID, fn, args...);
    }

    template<typename Tfn, typename ...Args>
    void add_transaction(BranchType::Enum btype, cv::Ptr<PlanContext> ctx, const string& txID, Tfn fn, Args ...args) {
        this->add_transaction(btype, [ctx](){ return ctx; }, txID, fn, args...);
    }

    template <typename ... Args, typename Tfn>
    static auto wrap_callable(Tfn fn) {
        if constexpr(CallableTraits<Tfn>::member_t::value) {
            return std::function([fn](Args... values) -> decltype(std::mem_fn(fn)(values...)) {
                return std::mem_fn(fn)(values...);
            });
        } else {
            return std::function(fn);
        }
    }

    template<bool Tconst, typename T>
    auto makeInternalEdge(T& val) {
        if constexpr(Tconst) {
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
        if(currentNodes_.empty())
            return;
        if(currentNodes_.back()->name_ == name)
            found = currentNodes_.back();
    }

    template<typename Tinstance>
    cv::Ptr<Tinstance> self() {
        if(!self_)
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
        return plan->template self<Tplan>();
    }

public:
template<typename Tfn, typename ... Args>
const string make_id(string id, const string& name, Tfn fn, Args ... args) {
    std::stringstream ss;
    if(!id.empty())
        id = "::" + id;

    if constexpr(std::is_pointer<Tfn>::value) {
        ss << name << id << " [" << detail::int_to_hex(fn) << "] ";
    } else if constexpr(!std::is_same<typename CallableTraits<Tfn>::return_type_t, std::false_type>::value) {
        ss << name << id << " [" << detail::int_to_hex(reinterpret_cast<size_t>(detail::TFN::ptr(fn))) << "] ";
    } else if constexpr(std::is_same<Runtime::Keys::Enum, Tfn>::value ||
                        std::is_same<GlobalState::Keys::Enum, Tfn>::value ||
                        std::is_same<LocalState::Keys::Enum, Tfn>::value) {
        ss << name << id << " [" << size_t(fn) << "] ";
    } else {
        ss << name << id << " [fn] ";
    }

    ((ss << detail::demangle(typeid(typename std::remove_reference_t<decltype(args)>::ref_t).name())
         << "(" << detail::int_to_hex(args.id()) << ") "), ...);
    ss << "- " << detail::map_index(std::this_thread::get_id());

    while(transactions_.find(ss.str()) != transactions_.end()) {
        ss << '+';
    }
    return ss.str();
}

template<bool TmakeEdge=true, typename TfnOp, typename ... Args>
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

    if constexpr(hasReturn && TmakeEdge) {
        cv::Ptr<cv::Ptr<val_t>> retPtr = new cv::Ptr<val_t>(cv::Ptr<val_t>(), nullptr);
        std::function wrap = [op](cv::Ptr<val_t>& v, typename Args::ref_t ... values) mutable {
            if constexpr(std::is_pointer<ret_no_ref_t>::value) {
                v = cv::Ptr<val_t>(cv::Ptr<val_t>(), op(values...));
            } else if constexpr(std::is_lvalue_reference<ret_t>::value) {
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
    void makeGraph();
    void runGraph();
    void clearGraph();

    template<typename T>
    struct Property : detail::Edge<const T, false, true, true> {
        using parent_t = detail::Edge<const T, false, true, true>;
        Property(cv::Ptr<Plan> plan, const T& val) : parent_t(parent_t::make(plan, val)) {
            GlobalState::shared_vars().makeSharedVar(val);
        }
    };

    constexpr static auto always_ = []() { return true; };
    constexpr static auto isTrue_ = [](const bool& b) { return b; };
    constexpr static auto isFalse_ = [](const bool& b) { return !b; };
    constexpr static auto and_ = [](const bool& a, const bool& b) { return a && b; };
    constexpr static auto or_ = [](const bool& a, const bool& b) { return a || b; };

    CV_EXPORTS Plan();
    CV_EXPORTS virtual ~Plan();

    virtual void setup() {}
    virtual void infer() = 0;
    virtual void teardown() {}

    virtual std::string space() {
        if(!parent_.empty())
            return parent_ + "-" + name();
        else
            return name();
    }

    virtual std::string name() {
        return detail::demangle(typeid(*this).name());
    }

    virtual void setParentID(const string& parent) { parent_ = parent; }
    virtual std::string getParentID() { return parent_; }

    void setRuntime(cv::Ptr<Runtime> rt) { runtime_ = rt; }
    cv::Ptr<Runtime> getRuntime() { return runtime_; }

    // Context execution: run fn in a named context
    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of<EdgeBase, Tfn>::value, cv::Ptr<Plan>>::type
    ctx(const std::string& ctxName, Tfn fn, Args ... args) {
        auto ctxCb = [this, ctxName]() -> cv::Ptr<PlanContext> {
            auto c = runtime_->getContext(ctxName);
            if(c) return c;
            return runtime_->plainCtx();
        };
        auto argsTuple = std::make_tuple(args...);
        return call(ctxCb, ctxName, fn, std::forward<decltype(argsTuple)>(argsTuple),
                    std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
    }

    // Plain context execution (no special context)
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

    template <typename ... Args>
    cv::Ptr<Plan> plain(Args... args) {
        auto fn = [](typename Args::ref_t ...){};
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "plain", wrap, args...);
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        add_transaction(runtime_->plainCtx(), id, wrap, args...);
        return self<Plan>();
    }

    // Branching
    template <typename Tedge>
    typename std::enable_if<std::is_base_of_v<EdgeBase, Tedge>, cv::Ptr<Plan>>::type
    branch(Tedge edge) {
        auto wrap = wrap_callable<typename Tedge::ref_t>([](const bool& b){ return b; });
        const string id = make_id(this->space(), "branch", wrap);
        branchStack_.push_front({id, BranchType::PARALLEL});
        emit_access(id, R(*this));
        add_transaction(BranchType::PARALLEL, runtime_->plainCtx(), id, wrap, edge);
        return self<Plan>();
    }

    template <typename Tfn>
    typename std::enable_if<!std::is_base_of_v<EdgeBase, Tfn>, cv::Ptr<Plan>>::type
    branch(Tfn fn) {
        auto wrap = wrap_callable(fn);
        const string id = make_id(this->space(), "branch", fn);
        branchStack_.push_front({id, BranchType::PARALLEL});
        emit_access(id, R(*this));
        add_transaction(BranchType::PARALLEL, runtime_->plainCtx(), id, wrap);
        return self<Plan>();
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_integral_v<Tfn>, cv::Ptr<Plan>>::type
    branch(Tfn fn, Args ... args) {
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "branch", fn, args...);
        branchStack_.push_front({id, BranchType::PARALLEL});
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        add_transaction(BranchType::PARALLEL, runtime_->plainCtx(), id, wrap, args...);
        return self<Plan>();
    }

    template <typename Tfn, typename ... Args>
    cv::Ptr<Plan> branch(BranchType::Enum type, Tfn fn, Args ... args) {
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "branch-type" + std::to_string((int)type), fn, args...);
        branchStack_.push_front({id, type});
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        add_transaction(type, runtime_->plainCtx(), id, wrap, args...);
        return self<Plan>();
    }

    template <typename Tedge>
    typename std::enable_if<std::is_base_of_v<EdgeBase, Tedge>, cv::Ptr<Plan>>::type
    branch(BranchType::Enum type, Tedge edge) {
        auto wrap = wrap_callable<typename Tedge::ref_t>([](const bool& b){ return b; });
        const string id = make_id(this->space(), "branch", wrap);
        branchStack_.push_front({id, type});
        emit_access(id, R(*this));
        add_transaction(type, runtime_->plainCtx(), id, wrap, edge);
        return self<Plan>();
    }

    cv::Ptr<Plan> endBranch() {
        auto current = branchStack_.front();
        branchStack_.pop_front();
        string id = "[end]" + current.first;
        emit_access(id, R(*this));
        std::function functor = [](){ return true; };
        add_transaction(current.second, runtime_->plainCtx(), id, functor);
        return self<Plan>();
    }

    cv::Ptr<Plan> elseBranch() {
        auto current = branchStack_.front();
        string id = "[else]" + current.first;
        emit_access(id, R(*this));
        std::function functor = [](){ return true; };
        add_transaction(current.second, runtime_->plainCtx(), id, functor);
        return self<Plan>();
    }

    // Sub-plan composition
    template <typename TsubPlan>
    cv::Ptr<Plan> subInfer(cv::Ptr<TsubPlan> subPlan) {
        subPlan->setRuntime(runtime_);
        subPlan->infer();
        subPlan->makeGraph();
        std::copy(subPlan->accesses_.begin(), subPlan->accesses_.end(), std::inserter(accesses_, accesses_.end()));
        std::copy(subPlan->transactions_.begin(), subPlan->transactions_.end(), std::inserter(transactions_, transactions_.end()));
        subPlan->clearGraph();
        return self<Plan>();
    }

    template <typename TsubPlan>
    cv::Ptr<Plan> subSetup(cv::Ptr<TsubPlan> subPlan) {
        subPlan->setRuntime(runtime_);
        subPlan->setup();
        subPlan->makeGraph();
        std::copy(subPlan->accesses_.begin(), subPlan->accesses_.end(), std::inserter(accesses_, accesses_.end()));
        std::copy(subPlan->transactions_.begin(), subPlan->transactions_.end(), std::inserter(transactions_, transactions_.end()));
        subPlan->clearGraph();
        return self<Plan>();
    }

    template <typename TsubPlan>
    cv::Ptr<Plan> subTeardown(cv::Ptr<TsubPlan> subPlan) {
        subPlan->setRuntime(runtime_);
        subPlan->teardown();
        subPlan->makeGraph();
        std::copy(subPlan->accesses_.begin(), subPlan->accesses_.end(), std::inserter(accesses_, accesses_.end()));
        std::copy(subPlan->transactions_.begin(), subPlan->transactions_.end(), std::inserter(transactions_, transactions_.end()));
        subPlan->clearGraph();
        return self<Plan>();
    }

    // Edge constructors
    template<typename T>
    detail::Edge<T, false, true> R(const T& t) {
        return detail::Edge<T, false, true>::make(self<Plan>(), t);
    }

    template<typename T>
    detail::Edge<T, false, true, true> RS(const T& t) {
        if(!GlobalState::shared_vars().checkShared(*this, t)) {
            throw std::runtime_error("You declare a non-shared variable as shared.");
        }
        return detail::Edge<T, false, true, true>::make(self<Plan>(), t);
    }

    template<typename T>
    detail::Edge<T, false, false> RW(T& t) {
        return detail::Edge<T, false, false>::make(self<Plan>(), t);
    }

    template<typename T>
    detail::Edge<T, false, false, true> RWS(T& t) {
        if(!GlobalState::shared_vars().checkShared(*this, t)) {
            throw std::runtime_error("You declare a non-shared variable as shared.");
        }
        return detail::Edge<T, false, false, true>::make(self<Plan>(), t);
    }

    template<typename T>
    detail::Edge<T, true, true, true> CS(T& t) {
        if(GlobalState::shared_vars().checkShared(*this, t)) {
            return detail::Edge<T, true, true, true>::make(self<Plan>(), t);
        } else {
            throw std::runtime_error("You are trying to safe-copy a non-shared variable.");
        }
    }

    template<typename T>
    detail::Edge<cv::Ptr<T>, false, true, false, T, true> V(T t) {
        auto ptr = cv::makePtr<T>(t);
        return detail::Edge<decltype(ptr), false, true, false, T, true>::make(self<Plan>(), ptr);
    }

    template<typename Tval>
    Property<Tval> P(Runtime::Keys::Enum key) {
        const auto& ref = runtime_->get<Tval>(key);
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

    // Function edge
    template<typename Tfn, typename ... Args>
    auto F(Tfn src, Args&& ... args) {
        return make_op(src, args...);
    }

    // Property setter
    template<typename Tedge>
    cv::Ptr<Plan> set(const Runtime::Keys::Enum& key, const Tedge& e) {
        auto plan = self<Plan>();
        auto fn = [plan, key](decltype(e.ref()) v) {
            plan->runtime_->set(key, v);
        };
        const string id = make_id(this->space(), "set", fn, e);
        emit_access(id, R(*this));
        emit_access(id, e);
        std::function<void(decltype(e.ref()))> functor(fn);
        add_transaction(runtime_->plainCtx(), id, functor, e);
        return self<Plan>();
    }

    // Operators
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

    // Factory and run
    template<typename Tplan, typename ... Args>
    static cv::Ptr<Tplan> make(Args&& ... args) {
        Tplan* plan = new Tplan(std::forward<Args>(args)...);
        plan->template setActualTypeSize<Tplan>();
        return plan->template self<Tplan>();
    }

    template<typename Tplan, typename ... Args>
    static void run(int32_t workers, cv::Ptr<Runtime> runtime, Args&& ... args);

private:
    template <typename Tctx, typename Tfn, typename Tuple, size_t ... idx>
    cv::Ptr<Plan> call(Tctx ctx, const string& name, Tfn fn, Tuple&& args, std::index_sequence<idx...>) {
        const string id = make_id(this->space(), name, fn, std::get<idx>(args)...);
        emit_access(id, R(*this));
        (emit_access(id, std::get<idx>(args)), ...);
        auto wrap = wrap_callable<typename std::remove_reference<decltype(std::get<idx>(args))>::type::ref_t...>(fn);
        add_transaction(ctx, id, wrap, std::get<idx>(args)...);
        return self<Plan>();
    }
};

// Free-standing operator overloads for edges
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

