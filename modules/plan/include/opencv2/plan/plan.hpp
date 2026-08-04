// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
//
// This copy carries minimal fixes required by the ll2plan-generated programs.
// All changes are marked with [ll2plan-fix N] comments:
//   1: Edge::set -- by-value smart payloads (Plan::V) are now owned, not aliased
//   2: Plan::make_op -- result slots are properly allocated/owned
//   3: Plan::run -- barrier size derived from WORKERS_STARTED (shared by all callers)
//   4: worker-pinned branch overloads -- lambda takes edge ref types
#ifndef OPENCV_PLAN_PLAN_HPP_
#define OPENCV_PLAN_PLAN_HPP_

#include "runtime.hpp"
#include "transaction.hpp"
#include "util.hpp"

#include <deque>
#include <vector>
#include <map>
#include <string>
#include <thread>
#include <barrier>
#include <semaphore>
#include <iostream>

namespace plan {

class PLAN_EXPORTS Plan {
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

    Ptr<Runtime> runtime_ = Runtime::instance();
    std::string parent_;
    size_t parentOffset_ = 0;
    size_t parentActualTypeSize_ = 0;
    size_t actualTypeSize_ = 0;
    Ptr<Plan> self_;
    std::vector<std::tuple<std::string, bool, size_t>> accesses_;
    std::map<std::string, Ptr<Transaction>> transactions_;
    std::vector<Ptr<Node>> currentNodes_;
    std::vector<Ptr<Node>> allNodes_;
    std::deque<BranchState> branchStateStack_;
    std::deque<std::pair<string, BranchType::Enum>> branchStack_;

    template<typename Tedge>
    void emit_access(const string& context, Tedge tp) {
        accesses_.push_back(std::make_tuple(context, Tedge::read_t::value, tp.id()));
    }

    template<typename Tfn, typename ...Args>
    void add_transaction(std::function<Ptr<Context>()> ctxCb, string txID, Tfn fn, Args ...args) {
        auto tx = make_transaction(fn, args...);
        tx->setContextCallback(ctxCb);
        tx->setBranchType(BranchType::NONE);
        transactions_.insert({txID, tx});
    }

    template<typename Tfn, typename ...Args>
    void add_transaction(BranchType::Enum btype, std::function<Ptr<Context>()> ctxCb,
                         string txID, Tfn fn, Args ...args) {
        auto tx = make_transaction(fn, args...);
        tx->setContextCallback(ctxCb);
        tx->setBranchType(btype);
        transactions_.insert({txID, tx});
    }

    template<typename Tfn, typename ...Args>
    void add_transaction(Ptr<Context> ctx, const string& txID, Tfn fn, Args ...args) {
        this->add_transaction([ctx](){ return ctx; }, txID, fn, args...);
    }

    template<typename Tfn, typename ...Args>
    void add_transaction(BranchType::Enum btype, Ptr<Context> ctx,
                         const string& txID, Tfn fn, Args ...args) {
        this->add_transaction(btype, [ctx](){ return ctx; }, txID, fn, args...);
    }

    template <typename ... Args, typename Tfn>
    static auto wrap_callable(Tfn fn) {
        if constexpr (std::is_void<typename detail::CallableTraits<Tfn>::return_type_t>::value ||
                      std::is_same<typename detail::CallableTraits<Tfn>::return_type_t, std::false_type>::value) {
            if constexpr (detail::CallableTraits<Tfn>::member_t::value)
                return std::function([fn](Args... values) -> decltype(std::mem_fn(fn)(values...)) {
                    return std::mem_fn(fn)(values...); });
            else
                return std::function(fn);
        } else {
            if constexpr (detail::CallableTraits<Tfn>::member_t::value)
                return std::function([fn](Args... values) -> decltype(std::mem_fn(fn)(values...)) {
                    return std::mem_fn(fn)(values...); });
            else
                return std::function(fn);
        }
    }

    template<bool Tconst, typename T>
    auto makeInternalEdge(T& val) {
        if constexpr (Tconst) return R(val);
        else return RW(val);
    }

    template<typename T> void setActualTypeSize() { actualTypeSize_ = sizeof(T); }
    template<typename T> void setParentActualTypeSize() { parentActualTypeSize_ = sizeof(T); }
    void setParentOffset(size_t offset) { parentOffset_ = offset; }
    size_t getActualTypeSize() { return actualTypeSize_; }
    size_t getParentActualTypeSize() { return parentActualTypeSize_; }
    size_t getParentOffset() { return parentOffset_; }

    void findNode(const string& name, Ptr<Node>& found) {
        PLAN_Assert(!name.empty());
        if (currentNodes_.empty()) return;
        if (currentNodes_.back()->name_ == name)
            found = currentNodes_.back();
    }

    void makeGraph();
    void pf(const size_t& depth, const BranchState& current, const Ptr<Node> n);
    void runGraph();
    void clearGraph();

    template<typename Tinstance>
    Ptr<Tinstance> self() {
        if (!self_)
            self_ = nonOwning<Plan>(this);
        return std::dynamic_pointer_cast<Tinstance>(self_);
    }

    template<typename Tplan, typename Tparent, typename ... Args>
    static Ptr<Tplan> makeSubPlan(Tparent* parent, Args&& ... args) {
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
        std::stringstream ss;
        if (!id.empty()) id = "::" + id;
        if constexpr (std::is_pointer<Tfn>::value)
            ss << name << id << " [" << detail::int_to_hex(reinterpret_cast<size_t>(fn)) << "] ";
        else
            ss << name << id << " [" << detail::lambda_ptr_hex(std::forward<Tfn>(fn)) << "] ";
        ((ss << detail::demangle(typeid(typename std::remove_reference_t<decltype(args)>::ref_t).name())
             << "(" << detail::int_to_hex(args.id()) << ") "), ...);
        ss << "- " << detail::map_index(std::this_thread::get_id());
        while (transactions_.find(ss.str()) != transactions_.end())
            ss << '+';
        return ss.str();
    }

public:
    template<typename T>
    struct Property : detail::Edge<const T, false, true, true> {
        using parent_t = detail::Edge<const T, false, true, true>;
        Property(Ptr<Plan> plan, const T& val)
            : parent_t(parent_t::make(plan, val)) {
            GlobalState::shared_vars().makeSharedVar(val);
        }
    };

    constexpr static auto always_  = []() { return true; };
    constexpr static auto isTrue_  = [](const bool& b) { return b; };
    constexpr static auto isFalse_ = [](const bool& b) { return !b; };
    constexpr static auto and_     = [](const bool& a, const bool& b) { return a && b; };
    constexpr static auto or_      = [](const bool& a, const bool& b) { return a || b; };

    virtual ~Plan() { self_ = nullptr; }

    virtual void gui() {}
    virtual void setup() {}
    virtual void infer() = 0;
    virtual void teardown() {}

    virtual std::string space() {
        if (!parent_.empty()) return parent_ + "-" + name();
        return name();
    }
    virtual std::string name() { return detail::demangle(typeid(*this).name()); }
    virtual void setParentID(const string& parent) { parent_ = parent; }
    virtual std::string getParentID() { return parent_; }

    //! Execute a function in a named context
    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of<detail::EdgeBase, Tfn>::value, Ptr<Plan>>::type
    ctx(const std::string& ctxName, Tfn fn, Args ... args) {
        auto planPtr = self<Plan>();
        auto ctxCallback = [planPtr, ctxName]() {
            return planPtr->runtime_->getContext(ctxName);
        };
        auto argsTuple = std::make_tuple(args...);
        call(ctxCallback, ctxName, fn, std::forward<decltype(argsTuple)>(argsTuple),
             std::make_index_sequence<std::tuple_size<decltype(argsTuple)>::value>());
        return self<Plan>();
    }

    //! Execute in the plain (default) context
    template <typename ... Args>
    Ptr<Plan> plain(Args... args) {
        auto fn = [](typename Args::ref_t ...){};
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "plain", wrap, args...);
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        add_transaction(runtime_->plainCtx(), id, wrap, args...);
        return self<Plan>();
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_base_of<detail::EdgeBase, Tfn>::value, Ptr<Plan>>::type
    plain(Tfn fn, Args... args) {
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "plain", fn, args...);
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        add_transaction(runtime_->plainCtx(), id, wrap, args...);
        return self<Plan>();
    }

    //! Branching
    template <typename Tedge>
    typename std::enable_if<std::is_base_of_v<detail::EdgeBase, Tedge>, Ptr<Plan>>::type
    branch(Tedge edge) {
        auto wrap = wrap_callable<typename Tedge::ref_t>([](const bool& b){ return b; });
        const string id = make_id(this->space(), "branch", wrap);
        branchStack_.push_front({id, BranchType::PARALLEL});
        emit_access(id, R(*this));
        add_transaction(BranchType::PARALLEL, runtime_->plainCtx(), id, wrap, edge);
        return self<Plan>();
    }

    template <typename Tfn>
    typename std::enable_if<!std::is_base_of_v<detail::EdgeBase, Tfn>, Ptr<Plan>>::type
    branch(Tfn fn) {
        auto wrap = wrap_callable(fn);
        const string id = make_id(this->space(), "branch", fn);
        branchStack_.push_front({id, BranchType::PARALLEL});
        emit_access(id, R(*this));
        add_transaction(BranchType::PARALLEL, runtime_->plainCtx(), id, wrap);
        return self<Plan>();
    }

    template <typename Tfn, typename ... Args>
    typename std::enable_if<!std::is_integral_v<Tfn>, Ptr<Plan>>::type
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
    typename std::enable_if<!std::is_base_of<detail::EdgeBase, Tfn>::value, Ptr<Plan>>::type
    branch(int workerIdx, Tfn fn, Args ... args) {
        auto wrapInner = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "branch-pin" + std::to_string(workerIdx), fn, args...);
        branchStack_.push_front({id, BranchType::PARALLEL});
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        std::function<bool((typename Args::ref_t...))> wrap =
            // [ll2plan-fix 4] lambda must take the edge *reference* types
            [this, workerIdx, wrapInner](typename Args::ref_t ... innerArgs) {
                return LocalState::get<size_t>(LocalState::Keys::WORKER_INDEX) ==
                       static_cast<size_t>(workerIdx) && wrapInner(innerArgs...);
            };
        add_transaction(BranchType::PARALLEL, runtime_->plainCtx(), id, wrap, args...);
        return self<Plan>();
    }

    template <typename Tfn, typename ... Args>
    Ptr<Plan> branch(int workerIdx, BranchType::Enum type, Tfn fn, Args ... args) {
        auto wrapInner = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "branch-pin" + std::to_string(workerIdx), fn, args...);
        branchStack_.push_front({id, BranchType::PARALLEL});
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        std::function<bool((typename Args::ref_t...))> wrap =
            // [ll2plan-fix 4] lambda must take the edge *reference* types
            [this, workerIdx, wrapInner](typename Args::ref_t ... innerArgs) {
                return LocalState::get<size_t>(LocalState::Keys::WORKER_INDEX) ==
                       static_cast<size_t>(workerIdx) && wrapInner(innerArgs...);
            };
        add_transaction(type, runtime_->plainCtx(), id, wrap, args...);
        return self<Plan>();
    }

    template <typename Tedge>
    typename std::enable_if<std::is_base_of_v<detail::EdgeBase, Tedge>, Ptr<Plan>>::type
    branch(BranchType::Enum type, Tedge edge) {
        auto wrap = wrap_callable<typename Tedge::ref_t>([](const bool& b){ return b; });
        const string id = make_id(this->space(), "branch", wrap);
        branchStack_.push_front({id, type});
        emit_access(id, R(*this));
        add_transaction(type, runtime_->plainCtx(), id, wrap, edge);
        return self<Plan>();
    }

    template <typename Tfn, typename ... Args>
    Ptr<Plan> branch(BranchType::Enum type, Tfn fn, Args ... args) {
        auto wrap = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(), "branch-type" + std::to_string((int)type), fn, args...);
        branchStack_.push_front({id, type});
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        add_transaction(type, runtime_->plainCtx(), id, wrap, args...);
        return self<Plan>();
    }

    template <typename Tfn, typename ... Args>
    Ptr<Plan> branch(BranchType::Enum type, int workerIdx, Tfn fn, Args ... args) {
        auto wrapInner = wrap_callable<typename Args::ref_t ...>(fn);
        const string id = make_id(this->space(),
            "branch-type-pin" + std::to_string((int)type) + "-" + std::to_string(workerIdx), fn, args...);
        branchStack_.push_front({id, type});
        emit_access(id, R(*this));
        (emit_access(id, args), ...);
        std::function<bool((typename Args::ref_t...))> wrap =
            // [ll2plan-fix 4] lambda must take the edge *reference* types
            [this, workerIdx, wrapInner](typename Args::ref_t ... innerArgs) {
                return LocalState::get<size_t>(LocalState::Keys::WORKER_INDEX) ==
                       static_cast<size_t>(workerIdx) && wrapInner(innerArgs...);
            };
        add_transaction(type, runtime_->plainCtx(), id, wrap, args...);
        return self<Plan>();
    }

    Ptr<Plan> endBranch();
    Ptr<Plan> elseBranch();

    template <typename Tctx, typename Tfn, typename Tuple, size_t ... idx>
    Ptr<Plan> call(Tctx ctx, const string& name, Tfn fn, Tuple&& args,
                   std::index_sequence<idx...>) {
        const string id = make_id(this->space(), name, fn, std::get<idx>(args)...);
        emit_access(id, R(*this));
        (emit_access(id, std::get<idx>(args)), ...);
        auto wrap = wrap_callable<
            typename std::remove_reference<decltype(std::get<idx>(args))>::type::ref_t...>(fn);
        add_transaction(ctx, id, wrap, std::get<idx>(args)...);
        return self<Plan>();
    }

    //! Sub-plan integration
    template <typename TsubPlan>
    Ptr<Plan> subInfer(Ptr<TsubPlan> subPlan) {
        subPlan->infer();
        subPlan->makeGraph();
        std::copy(subPlan->accesses_.begin(), subPlan->accesses_.end(),
                  std::inserter(accesses_, accesses_.end()));
        std::copy(subPlan->transactions_.begin(), subPlan->transactions_.end(),
                  std::inserter(transactions_, transactions_.end()));
        subPlan->clearGraph();
        return self<Plan>();
    }

    template <typename TsubPlan>
    Ptr<Plan> subSetup(Ptr<TsubPlan> subPlan) {
        subPlan->setup();
        subPlan->makeGraph();
        std::copy(subPlan->accesses_.begin(), subPlan->accesses_.end(),
                  std::inserter(accesses_, accesses_.end()));
        std::copy(subPlan->transactions_.begin(), subPlan->transactions_.end(),
                  std::inserter(transactions_, transactions_.end()));
        subPlan->clearGraph();
        return self<Plan>();
    }

    template <typename TsubPlan>
    Ptr<Plan> subTeardown(Ptr<TsubPlan> subPlan) {
        subPlan->teardown();
        subPlan->makeGraph();
        std::copy(subPlan->accesses_.begin(), subPlan->accesses_.end(),
                  std::inserter(accesses_, accesses_.end()));
        std::copy(subPlan->transactions_.begin(), subPlan->transactions_.end(),
                  std::inserter(transactions_, transactions_.end()));
        subPlan->clearGraph();
        return self<Plan>();
    }

    //! Set properties
    template <typename TwrapFn, typename Ttuple, size_t ... idx>
    Ptr<Plan> set(const string& id, TwrapFn fn, Ttuple&& args, std::index_sequence<idx...>) {
        auto wrap = wrap_callable<decltype(std::get<0>(args))>(fn);
        emit_access(id, R(*this));
        (emit_access(id, std::get<idx>(args)), ...);
        add_transaction(runtime_->plainCtx(), id, wrap, std::get<idx>(args)...);
        return self<Plan>();
    }

    template <typename Tedge, size_t ... idx>
    Ptr<Plan> set(std::tuple<Runtime::Keys::Enum, Tedge>&& values, std::index_sequence<idx...>) {
        const string id = make_id(this->space(), "set-fn", std::get<idx>(values)...);
        std::function wrap = make_setter_function(values);
        auto args = detail::sub_tuple<1,
            std::tuple_size<std::tuple<Runtime::Keys::Enum, Tedge>>::value - 1>(values);
        return set(id, wrap, std::forward<decltype(args)>(args),
                   std::make_index_sequence<std::tuple_size<decltype(args)>::value>());
    }

    template <typename Tedge>
    Ptr<Plan> set(std::tuple<Runtime::Keys::Enum, Tedge>&& values) {
        using sz = std::tuple_size<std::tuple<Runtime::Keys::Enum, Tedge>>;
        return set(std::forward<std::tuple<Runtime::Keys::Enum, Tedge>>(values),
                   std::make_index_sequence<sz::value>());
    }

    template<typename Tedge>
    Ptr<Plan> set(const Runtime::Keys::Enum& key, const Tedge& e) {
        return set(std::make_tuple(key, e), std::make_index_sequence<1>());
    }

    template<typename ... Args>
    Ptr<Plan> set(std::tuple<Runtime::Keys::Enum, Args>&& ... tuples) {
        (set(std::forward<std::tuple<Runtime::Keys::Enum, Args>>(tuples)), ...);
        return self<Plan>();
    }

    //! Operators
    template<detail::Operators Top, typename ... Edges>
    Ptr<Plan> op(Edges ... edges) {
        return make_op<false>(
            detail::make_operator_func<detail::check_op<Top, Edges...>::value>(edges...), edges...);
    }

    template<typename ... Edges>
    Ptr<Plan> assign(Edges ... edges) {
        return make_op<false>(
            detail::make_operator_func<detail::check_op<detail::ASSIGN_, Edges...>::value>(edges...), edges...);
    }

    template<typename ... Edges>
    Ptr<Plan> construct(Edges ... edges) {
        return make_op<false>(
            detail::make_operator_func<detail::check_op<detail::CONSTRUCT_, Edges...>::value>(edges...), edges...);
    }

    template<detail::Operators Top, typename ... Edges>
    auto OP(Edges ... edges) {
        return make_op(
            detail::make_operator_func<detail::check_op<Top, Edges...>::value>(edges...), edges...);
    }

    template<typename ... Edges> auto operator()(Edges&& ... edges) { return OP<detail::Operators::CONSTRUCT_>(edges...); }
    template<typename ... Edges> auto IF(Edges&& ... edges)    { return OP<detail::Operators::IF_>(edges...); }
    template<typename ... Edges> auto ASSIGN(Edges&& ... edges){ return OP<detail::Operators::ASSIGN_>(edges...); }
    template<typename ... Edges> auto SUB(Edges&& ... edges)   { return OP<detail::Operators::SUB_>(edges...); }
    template<typename ... Edges> auto MUL(Edges&& ... edges)   { return OP<detail::Operators::MUL_>(edges...); }
    template<typename ... Edges> auto DIV(Edges&& ... edges)   { return OP<detail::Operators::DIV_>(edges...); }
    template<typename ... Edges> auto MOD(Edges&& ... edges)   { return OP<detail::Operators::MOD_>(edges...); }
    template<typename ... Edges> auto INCL(Edges&& ... edges)  { return OP<detail::Operators::INCL_>(edges...); }
    template<typename ... Edges> auto INCR(Edges&& ... edges)  { return OP<detail::Operators::INCR_>(edges...); }
    template<typename ... Edges> auto DECL(Edges&& ... edges)  { return OP<detail::Operators::DECL_>(edges...); }
    template<typename ... Edges> auto DECR(Edges&& ... edges)  { return OP<detail::Operators::DECR_>(edges...); }
    template<typename ... Edges> auto AND(Edges&& ... edges)   { return OP<detail::Operators::AND_>(edges...); }
    template<typename ... Edges> auto OR(Edges&& ... edges)    { return OP<detail::Operators::OR_>(edges...); }
    template<typename ... Edges> auto EQ(Edges&& ... edges)    { return OP<detail::Operators::EQ_>(edges...); }
    template<typename ... Edges> auto NEQ(Edges&& ... edges)   { return OP<detail::Operators::NEQ_>(edges...); }
    template<typename ... Edges> auto LT(Edges&& ... edges)    { return OP<detail::Operators::LT_>(edges...); }
    template<typename ... Edges> auto GT(Edges&& ... edges)    { return OP<detail::Operators::GT_>(edges...); }
    template<typename ... Edges> auto LE(Edges&& ... edges)    { return OP<detail::Operators::LE_>(edges...); }
    template<typename ... Edges> auto GE(Edges&& ... edges)    { return OP<detail::Operators::GE_>(edges...); }
    template<typename ... Edges> auto NOT(Edges&& ... edges)   { return OP<detail::Operators::NOT_>(edges...); }
    template<typename ... Edges> auto XOR(Edges&& ... edges)   { return OP<detail::Operators::XOR_>(edges...); }
    template<typename ... Edges> auto BAND(Edges&& ... edges)  { return OP<detail::Operators::BAND_>(edges...); }
    template<typename ... Edges> auto BOR(Edges&& ... edges)   { return OP<detail::Operators::BOR_>(edges...); }
    template<typename ... Edges> auto SHL(Edges&& ... edges)   { return OP<detail::Operators::SHL_>(edges...); }
    template<typename ... Edges> auto SHR(Edges&& ... edges)   { return OP<detail::Operators::SHR_>(edges...); }

    template<typename Tfn, typename ... Args>
    auto F(Tfn src, Args&& ... args) { return make_op(src, args...); }

    template<typename ... Args>
    auto _(Args&& ... args) { return std::make_tuple(std::forward<const Args>(args)...); }

    template<typename TsubPlan, typename Tparent, typename ... Args>
    auto _sub(Tparent* parent, Args&& ... args) {
        return Plan::makeSubPlan<TsubPlan>(parent, std::forward<Args>(args)...);
    }

    template<typename TsubPlan, typename TparentPtr, typename ... Args>
    auto _sub(TparentPtr parent, Args&& ... args) {
        return Plan::makeSubPlan<TsubPlan>(parent.get(), std::forward<Args>(args)...);
    }

    template<typename Tvar> void _shared(Tvar& val) { GlobalState::shared_vars().makeSharedVar(val); }
    template<typename Tvar> void _safe(Tvar& val)   { GlobalState::shared_vars().registerSafe(val); }

    //! Edge creation
    template<typename T>
    detail::Edge<T, false, true> R(const T& t) {
        return detail::Edge<T, false, true>::make(self<Plan>(), t);
    }

    template<typename T>
    detail::Edge<T, false, true, true> RS(const T& t) {
        if (!GlobalState::shared_vars().checkShared(*this, t))
            throw std::runtime_error("You declare a non-shared variable as shared.");
        return detail::Edge<T, false, true, true>::make(self<Plan>(), t);
    }

    template<typename T>
    detail::Edge<T, false, false> RW(T& t) {
        return detail::Edge<T, false, false>::make(self<Plan>(), t);
    }

    template<typename T>
    detail::Edge<T, false, false, true> RWS(T& t) {
        if (!GlobalState::shared_vars().checkShared(*this, t))
            throw std::runtime_error("You declare a non-shared variable as shared.");
        return detail::Edge<T, false, false, true>::make(self<Plan>(), t);
    }

    template<typename T>
    detail::Edge<T, true, true, true> CS(T& t) {
        if (GlobalState::shared_vars().checkShared(*this, t))
            return detail::Edge<T, true, true, true>::make(self<Plan>(), t);
        throw std::runtime_error("You are trying to safe-copy a non-shared variable.");
    }

    template<typename T>
    detail::Edge<Ptr<T>, false, true, false, T, true> V(T t) {
        auto ptr = makePtr<T>(t);
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

    //! Factory and run
    template<typename Tplan, typename ... Args>
    static Ptr<Tplan> make(Args&& ... args) {
        Tplan* plan = new Tplan(std::forward<Args>(args)...);
        plan->template setActualTypeSize<Tplan>();
        Runtime::set(Runtime::Keys::NAMESPACE, plan->space());
        return plan->template self<Tplan>();
    }

    template<typename Tplan, typename ... Args>
    static void run(int32_t workers, Args&& ... args) {
        PLAN_Assert(workers > -2);
        if (workers == -1) workers = 2;
        else ++workers;

        Ptr<Tplan> plan;
        static std::mutex worker_init_mtx_;
        std::vector<std::thread*> threads;
        {
            static std::mutex runMtx;
            std::lock_guard<std::mutex> lock(runMtx);
            if (GlobalState::isFirstRun())
                GlobalState::setMainID(std::this_thread::get_id());
            plan = make<Tplan>(std::forward<Args>(args)...);
            if (GlobalState::isMain()) {
                GlobalState::set<size_t>(GlobalState::Keys::WORKERS_STARTED, workers);
                for (int32_t i = 0; i < workers; ++i) {
                    threads.push_back(new std::thread(
                        [plan, i, argsTuple = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                            string name = "plan-worker-" + std::to_string(i);
                            setThreadName(name.c_str());
                            {
                                std::lock_guard guard(worker_init_mtx_);
                                Runtime::init(plan->runtime_->size(), name);
                            }
                            LocalState::set(LocalState::Keys::WORKER_INDEX, size_t(i));
                            std::apply([](auto&&... a) {
                                Plan::run<Tplan>(0, std::forward<decltype(a)>(a)...);
                            }, std::move(argsTuple));
                        }));
                }
            }
        }
        PLAN_Assert(plan);

        if (!GlobalState::isMain()) {
            static std::binary_semaphore setup_sema(1);
            try {
                setup_sema.acquire();
                plan->setup();
                plan->makeGraph();
                plan->runGraph();
                plan->clearGraph();
                setup_sema.release();
            } catch (std::exception& ex) {
                throw std::runtime_error(std::string("Setup failed: ") + ex.what());
            }
        }

        if (GlobalState::isMain()) {
            try { plan->gui(); }
            catch (std::exception& ex) {
                throw std::runtime_error(std::string("Loading GUI failed: ") + ex.what());
            }
        } else {
            try {
                plan->infer();
                plan->makeGraph();
            } catch (std::exception& ex) {
                throw std::runtime_error(std::string("Main inference failed: ") + ex.what());
            }
            GlobalState::apply<size_t>(GlobalState::Keys::WORKERS_READY,
                                       [](size_t& wr){ ++wr; return wr; });
        }

        // [ll2plan-fix 3] The barrier size must be identical for the main thread
        // and every worker thread. The published WORKERS_STARTED count is the
        // single source of truth (the local `workers` value differs per caller).
        static std::barrier syncPoint(std::ptrdiff_t(
            GlobalState::get<size_t>(GlobalState::Keys::WORKERS_STARTED) + 1));
        syncPoint.arrive_and_wait();

        try {
            Runtime::run(plan->runtime_, [plan](){ plan->runGraph(); });
        } catch (std::exception& ex) {
            throw std::runtime_error(std::string("Main runtime failed: ") + ex.what());
        }

        if (!GlobalState::isMain()) {
            plan->clearGraph();
            try {
                plan->teardown();
                plan->makeGraph();
                plan->runGraph();
                plan->clearGraph();
            } catch (std::exception& ex) {
                throw std::runtime_error(std::string("Pipeline teardown failed: ") + ex.what());
            }
        } else {
            for (auto& t : threads) t->join();
        }
    }

private:
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
        if constexpr (!detail::is_callable<decltype(val)>::value) {
            static_assert(sz == 2, "Can not set a Property from multiple Edges");
            auto plan = self<Plan>();
            return std::function([plan, key](decltype(val.ref()) v) { Runtime::set(key, v); });
        } else {
            auto fn = std::get<1>(values);
            auto args = detail::sub_tuple<1, sz - 1>(values);
            auto plan = self<Plan>();
            return make_setter_function(key, fn, args, std::make_index_sequence<sz - 2>());
        }
    }

    template<bool TmakeEdge = true, typename TfnOp, typename ... Args>
    auto make_op(TfnOp fn, Args ... args) {
        auto op = wrap_callable<typename Args::ref_t ...>(fn);
        using ret_t = typename detail::CallableTraits<decltype(op)>::return_type_t;
        constexpr bool hasReturn = !std::is_same<ret_t, void>::value;
        using ret_no_ref_t = typename std::remove_reference<ret_t>::type;
        static_assert(!std::is_same<ret_no_ref_t, std::false_type>::value,
                      "Invalid callable passed to Plan::op");
        using val_t = typename std::disjunction<
            detail::values_equal<hasReturn, true, typename std::remove_pointer<ret_no_ref_t>::type>,
            detail::default_type<int>
        >::type;
        if constexpr (hasReturn && TmakeEdge) {
            // [ll2plan-fix 2] The old code constructed the result slot with the
            // shared_ptr aliasing constructor over an empty owner, leaving
            // retPtr.get() == nullptr. Own the slot properly and keep it alive
            // by capturing retPtr inside the transaction callable.
            Ptr<Ptr<val_t>> retPtr = std::make_shared<Ptr<val_t>>();
            std::function wrap = [op, retPtr](Ptr<val_t>& v, typename Args::ref_t ... values) mutable {
                if constexpr (std::is_pointer<ret_no_ref_t>::value)
                    v = Ptr<val_t>(Ptr<val_t>(), op(values...));
                else if constexpr (std::is_lvalue_reference<ret_t>::value) {
                    auto& ref = op(values...);
                    v = Ptr<val_t>(Ptr<val_t>(), std::addressof(ref));
                } else
                    v = std::make_shared<val_t>(op(values...));
            };
            const string id = make_id(this->space(), "nary-op", wrap, args...);
            emit_access(id, R(*this));
            (emit_access(id, args), ...);
            auto ptrEdge = detail::Edge<Ptr<Ptr<val_t>>, false, false, false, Ptr<val_t>, true>::make(
                self<Plan>(), retPtr);
            add_transaction(runtime_->plainCtx(), id, wrap, ptrEdge, args...);
            return detail::Edge<Ptr<val_t>, false, false, false, val_t>::make(
                self<Plan>(), *retPtr.get());
        } else {
            std::function wrap = [op](typename Args::ref_t ... values) { op(values...); };
            const string id = make_id(this->space(), "nary-op", wrap, args...);
            emit_access(id, R(*this));
            (emit_access(id, args), ...);
            add_transaction(runtime_->plainCtx(), id, wrap, args...);
            return self<Plan>();
        }
    }
};

// ---- Free-standing operator overloads ----

template<typename ... Edges>
auto operator+(const std::tuple<Edges...>& t) { return detail::Operation::op<detail::ADD_>(t); }
template<typename TedgeL, typename ... Edges>
auto operator+(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::ADD_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator+(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator+(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator+(const TedgeL& l, const Plan::Property<T>& r) {
    return operator+(l, std::make_tuple(r));
}

template<typename ... Edges>
auto operator-(const std::tuple<Edges...>& t) { return detail::Operation::op<detail::SUB_>(t); }
template<typename TedgeL, typename ... Edges>
auto operator-(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::SUB_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator-(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator-(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator-(const TedgeL& l, const Plan::Property<T>& r) {
    return operator-(l, std::make_tuple(r));
}
template<typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator-(const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator*(r, std::make_tuple(std::forward<decltype(r.plan()->V(-1))>(r.plan()->V(-1))));
}
template<typename T>
auto operator-(const Plan::Property<T>& r) {
    return operator*(r, std::make_tuple(std::forward<decltype(r.plan()->V(-1))>(r.plan()->V(-1))));
}

template<typename TedgeL, typename ... Edges>
auto operator*(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::MUL_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator*(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator*(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator*(const TedgeL& l, const Plan::Property<T>& r) {
    return operator*(l, std::make_tuple(r));
}

template<typename TedgeL, typename ... Edges>
auto operator/(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::DIV_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator/(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator/(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator/(const TedgeL& l, const Plan::Property<T>& r) {
    return operator/(l, std::make_tuple(r));
}

template<typename TedgeL, typename ... Edges>
auto operator%(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::MOD_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator%(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator%(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator%(const TedgeL& l, const Plan::Property<T>& r) {
    return operator%(l, std::make_tuple(r));
}

template<typename ... Edges>
auto operator++(const std::tuple<Edges...>& t) { return detail::Operation::op<detail::INCL_>(t); }
template<typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator++(const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& e) { return operator++(std::make_tuple(e)); }
template<typename T> auto operator++(const Plan::Property<T>& r) { return operator++(std::make_tuple(r)); }
template<typename ... Edges>
auto operator++(const std::tuple<Edges...>& t, int) { return detail::Operation::op<detail::INCR_>(t); }
template<typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator++(const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& e, int) { return operator++(std::make_tuple(e)); }
template<typename T> auto operator++(const Plan::Property<T>& r, int) { return operator++(std::make_tuple(r)); }

template<typename ... Edges>
auto operator--(const std::tuple<Edges...>& t) { return detail::Operation::op<detail::DECL_>(t); }
template<typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator--(const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& e) { return operator--(std::make_tuple(e)); }
template<typename T> auto operator--(const Plan::Property<T>& r) { return operator--(std::make_tuple(r)); }
template<typename ... Edges>
auto operator--(const std::tuple<Edges...>& t, int) { return detail::Operation::op<detail::DECR_>(t); }
template<typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator--(const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& e, int) { return operator--(std::make_tuple(e)); }
template<typename T> auto operator--(const Plan::Property<T>& r, int) { return operator--(std::make_tuple(r)); }

template<typename TedgeL, typename ... Edges>
auto operator&&(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::AND_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator&&(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator&&(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator&&(const TedgeL& l, const Plan::Property<T>& r) { return operator&&(l, std::make_tuple(r)); }

template<typename TedgeL, typename ... Edges>
auto operator||(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::OR_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator||(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator||(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator||(const TedgeL& l, const Plan::Property<T>& r) { return operator||(l, std::make_tuple(r)); }

template<typename TedgeL, typename ... Edges>
auto operator==(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::EQ_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator==(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator==(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator==(const TedgeL& l, const Plan::Property<T>& r) { return operator==(l, std::make_tuple(r)); }

template<typename TedgeL, typename ... Edges>
auto operator!=(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::NEQ_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator!=(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator!=(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator!=(const TedgeL& l, const Plan::Property<T>& r) { return operator!=(l, std::make_tuple(r)); }

template<typename TedgeL, typename ... Edges>
auto operator<(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::LT_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator<(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator<(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator<(const TedgeL& l, const Plan::Property<T>& r) { return operator<(l, std::make_tuple(r)); }

template<typename TedgeL, typename ... Edges>
auto operator>(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::GT_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator>(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator>(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator>(const TedgeL& l, const Plan::Property<T>& r) { return operator>(l, std::make_tuple(r)); }

template<typename TedgeL, typename ... Edges>
auto operator<=(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::LE_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator<=(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator<=(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator<=(const TedgeL& l, const Plan::Property<T>& r) { return operator<=(l, std::make_tuple(r)); }

template<typename TedgeL, typename ... Edges>
auto operator>=(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::GE_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator>=(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator>=(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator>=(const TedgeL& l, const Plan::Property<T>& r) { return operator>=(l, std::make_tuple(r)); }

template<typename ... Edges>
auto operator!(const std::tuple<Edges...>& t) { return detail::Operation::op<detail::NOT_>(t); }
template<typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator!(const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& e) { return operator!(std::make_tuple(e)); }
template<typename T> auto operator!(const Plan::Property<T>& r) { return operator!(std::make_tuple(r)); }

template<typename TedgeL, typename ... Edges>
auto operator^(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::XOR_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator^(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator^(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator^(const TedgeL& l, const Plan::Property<T>& r) { return operator^(l, std::make_tuple(r)); }

template<typename TedgeL, typename ... Edges>
auto operator&(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::BAND_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator&(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator&(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator&(const TedgeL& l, const Plan::Property<T>& r) { return operator&(l, std::make_tuple(r)); }

template<typename TedgeL, typename ... Edges>
auto operator|(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::BOR_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator|(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator|(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator|(const TedgeL& l, const Plan::Property<T>& r) { return operator|(l, std::make_tuple(r)); }

template<typename TedgeL, typename ... Edges>
auto operator<<(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::SHL_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator<<(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator<<(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator<<(const TedgeL& l, const Plan::Property<T>& r) { return operator<<(l, std::make_tuple(r)); }

template<typename TedgeL, typename ... Edges>
auto operator>>(const TedgeL& l, const std::tuple<Edges...>& t) {
    return detail::Operation::op<detail::SHR_>(std::tuple_cat(std::make_tuple(l), t));
}
template<typename TedgeL, typename Te, bool Tc, bool Tr, bool Ts, typename Tb, bool TbV>
auto operator>>(const TedgeL& l, const detail::Edge<Te,Tc,Tr,Ts,Tb,TbV>& r) {
    return operator>>(l, std::make_tuple(r));
}
template<typename TedgeL, typename T>
auto operator>>(const TedgeL& l, const Plan::Property<T>& r) { return operator>>(l, std::make_tuple(r)); }

} // namespace plan

#endif // OPENCV_PLAN_PLAN_HPP_

