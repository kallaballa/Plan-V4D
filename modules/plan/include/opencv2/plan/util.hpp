#ifndef OPENCV_PLAN_UTIL_HPP_
#define OPENCV_PLAN_UTIL_HPP_

#include <filesystem>
#include <string>
#include <iostream>
#include <array>
#include <set>
#include <map>
#include <mutex>
#include <functional>
#include <cmath>
#include <thread>
#include <deque>
#include <type_traits>
#include <tuple>
#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>

#include "opencv2/plan/threadsafeanymap.hpp"

namespace cv {
namespace plan {

#define _OLM_(r,c,f, ...) static_cast<r (c::*)(__VA_ARGS__)>(f)
#define _OLMC_(r,c,f, ...) static_cast<r (c::*)(__VA_ARGS__) const>(f)
#define _OL_(r,f, ...) static_cast<r (*)(__VA_ARGS__)>(f)
#define _OLC_(r,f, ...) static_cast<r (*)(__VA_ARGS__) const>(f)

namespace detail {

template<auto V1, decltype(V1) V2, typename T>
struct values_equal : std::bool_constant<V1 == V2> {
    using type = T;
};

template<typename T>
struct default_type : std::true_type {
    using type = T;
};

template <typename, typename = void>
struct has_call_operator_t : std::false_type {};

template <typename T>
struct has_call_operator_t<T, std::void_t<decltype(&T::operator())>> : std::is_same<std::true_type, std::true_type> {};

template <typename, typename = void>
struct has_return_type_t : std::false_type {};

template <typename T>
struct has_return_type_t<T, std::void_t<decltype(&T::return_type)>> : std::is_same<std::true_type, std::true_type> {};

template <template <typename...> class Template, typename T>
struct is_specialization_of : std::false_type {};

template <template <typename...> class Template, typename... Args>
struct is_specialization_of<Template, Template<Args...>> : std::true_type {};

template<typename T>
struct is_callable : public std::disjunction<
    std::disjunction<has_call_operator_t<T>, std::is_pointer<T>>,
    std::is_member_function_pointer<T>,
    std::is_function<T>> {};

template<class T>
struct function_traits : function_traits<decltype(&T::operator())> {};

template<>
struct function_traits<std::false_type> : std::false_type {
    using result_type = std::false_type;
};

template<class R, class... Args>
struct function_traits<R(Args...)> {
    using result_type = R;
    using argument_types = std::tuple<std::remove_reference_t<Args>...>;
    static const bool value = true;
};

template<class R, class... Args>
struct function_traits<R (*)(Args...)> {
    using result_type = R;
    using argument_types = std::tuple<std::remove_reference_t<Args>...>;
    static const bool value = true;
};

template<class R, class... Args>
struct function_traits<std::function<R(Args...)>> {
    using result_type = R;
    using argument_types = std::tuple<std::remove_reference_t<Args>...>;
    static const bool value = true;
};

template<class T, class R, class... Args>
struct function_traits<R (T::*)(Args...)> {
    using result_type = R;
    using argument_types = std::tuple<std::remove_reference_t<Args>...>;
    static const bool value = true;
};

template<class T, class R, class... Args>
struct function_traits<R (T::*)(Args...) const> {
    using result_type = R;
    using argument_types = std::tuple<std::remove_reference_t<Args>...>;
    static const bool value = true;
};

template <typename, typename = void>
struct element_t : std::false_type {
    using type = std::false_type;
};

template <typename Tptr>
struct element_t<Tptr, std::void_t<decltype(&Tptr::get)>> : std::is_same<std::true_type, std::true_type> {
    using type = std::remove_pointer_t<typename Tptr::element_type>;
};

template <typename, typename = void>
struct return_t : std::false_type {
    using type = std::false_type;
};

template <typename Tfn>
struct return_t<Tfn, std::void_t<decltype(&Tfn::operator())>> : std::is_same<std::true_type, std::true_type> {
    using type = typename function_traits<Tfn>::result_type;
};

template <typename T>
struct CallableTraits {
    using return_type_t = typename detail::return_t<T>::type;
    using member_t = std::false_type;
    using object_t = std::false_type;
    using args_t = std::false_type;
};

template <typename Return, typename Object>
struct CallableTraits<Return Object::*> {
    using return_type_t = Return;
    using member_t = std::true_type;
    using object_t = Object;
    using args_t = std::false_type;
};

template <typename Return, typename Object, typename... Args>
struct CallableTraits<Return (Object::*)(Args...)> {
    using return_type_t = Return;
    using member_t = std::true_type;
    using object_t = Object;
    using args_t = std::tuple<Args...>;
};

template <typename Return, typename... Args>
struct CallableTraits<Return (*)(Args...)> {
    using return_type_t = Return;
    using member_t = std::false_type;
    using object_t = std::false_type;
    using args_t = std::tuple<Args...>;
};

template <typename Return, typename... Args>
struct CallableTraits<Return(Args...)> {
    using member_t = std::false_type;
    using return_type_t = Return;
    using object_t = std::false_type;
    using args_t = std::tuple<Args...>;
};

template <size_t offset, size_t len, class tuple, size_t ... idx>
auto sub_tuple(tuple&& t, std::index_sequence<idx...>) {
    static_assert(offset + len <= std::tuple_size<typename std::remove_reference<tuple>::type>::value, "sub tuple is out of bounds!");
    return std::make_tuple(std::get<idx + offset>(t)...);
}

template <size_t offset, size_t len, class tuple>
auto sub_tuple(tuple&& t) {
    return sub_tuple<offset, len, tuple>(std::forward<tuple>(t), std::make_index_sequence<len>());
}

class TFN {
    template<typename T>
    static const void* fn(const void* new_fn = nullptr) {
        CV_Assert(new_fn);
        return new_fn;
    }

    template<typename Tret, typename T>
    static Tret tfn_ptr_exec() {
        return (Tret)(*(T*)fn<T>());
    }

public:
    template<typename Tret = void, typename Tfp = Tret(*)(), typename T>
    static Tfp ptr(T& t) {
        static std::mutex tfn_mtx;
        std::scoped_lock lock(tfn_mtx);
        fn<T>(&t);
        return (Tfp)tfn_ptr_exec<Tret, T>;
    }
};

template<bool read, typename Tfn, typename ... Args>
struct edgefun_t {
    edgefun_t(Tfn fn, Args ... args) {}
    using return_type_t = typename CallableTraits<Tfn>::return_type_t;
    static_assert(!std::is_same<return_type_t, std::false_type>::value, "Invalid callable passed");
    using type = typename std::disjunction<
        default_type<std::function<return_type_t(typename Args::ref_t ...)>>
    >::type;
};

template<typename T>
std::string int_to_hex(T i) {
    std::stringstream stream;
    stream << "0x"
           << std::setfill('0') << std::setw(sizeof(T) * 2)
           << std::hex << i;
    return stream.str();
}

static std::size_t map_index(const std::thread::id id) {
    static std::size_t nextindex = 0;
    static std::mutex my_mutex;
    static std::unordered_map<std::thread::id, std::size_t> ids;
    std::lock_guard<std::mutex> lock(my_mutex);
    auto iter = ids.find(id);
    if(iter == ids.end())
        return ids[id] = nextindex++;
    return iter->second;
}

} // namespace detail

using std::string;

inline uint64_t get_epoch_nanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

class SharedVariables {
    std::mutex sharedVarsMtx_;
    std::mutex safeVarsMtx_;
    std::map<size_t, std::pair<size_t, cv::Ptr<std::mutex>>> sharedVars_;
    std::map<size_t, std::pair<size_t, cv::Ptr<std::mutex>>> safeVars_;
    typedef typename std::map<size_t, std::pair<size_t, cv::Ptr<std::mutex>>>::iterator SharedVarsIter;

    template<typename T>
    std::pair<size_t, size_t> findSharedParent(const T& shared) {
        off_t varOffset = reinterpret_cast<size_t>(&shared);
        off_t registeredOffset = 0;
        off_t registeredSize = 0;
        for(const auto& p : sharedVars_) {
            registeredOffset = p.first;
            if(varOffset > registeredOffset) {
                registeredSize = p.second.first;
                if(varOffset < (registeredOffset + registeredSize)) {
                    return {static_cast<size_t>(registeredOffset), static_cast<size_t>(registeredSize)};
                }
            }
        }
        return {0, 0};
    }

public:
    template<typename Tplan, typename Tvar>
    static bool isPlanMember(Tplan& plan, Tvar& var) {
        const char* planPtr = reinterpret_cast<const char*>(&plan);
        const char* varPtr = reinterpret_cast<const char*>(&var);
        off_t parentOffset = plan.getParentOffset();
        off_t parentActualSize = plan.getParentActualTypeSize();
        off_t actualTypeSize = plan.getActualTypeSize();
        off_t varOffset = off_t(varPtr);
        off_t planOffset = off_t(planPtr);

        CV_Assert((parentOffset == 0 && parentActualSize == 0) || (parentOffset > 0 && parentActualSize > 0));

        off_t parentLowerBound = parentOffset;
        off_t parentUpperBound = parentOffset + parentActualSize;
        off_t lowerBound = planOffset;
        off_t upperBound = planOffset + actualTypeSize;

        if(!((varOffset >= lowerBound && varOffset <= upperBound) ||
             (parentOffset > 0 && varOffset >= parentLowerBound && varOffset <= parentUpperBound))) {
            return false;
        }
        return true;
    }

    template<typename T>
    void makeSharedVar(const T& candidate) {
        {
            std::lock_guard<std::mutex> guard(safeVarsMtx_);
            CV_Assert(safeVars_.find(reinterpret_cast<size_t>(&candidate)) == safeVars_.end());
        }
        std::lock_guard<std::mutex> guard(sharedVarsMtx_);
        if(sharedVars_.find(reinterpret_cast<size_t>(&candidate)) != sharedVars_.end()) {
            return;
        } else {
            auto parent = findSharedParent(candidate);
            if(parent.first != 0) {
                auto it = sharedVars_.find(parent.first);
                CV_Assert(it != sharedVars_.end());
                sharedVars_.insert({reinterpret_cast<size_t>(&candidate), std::make_pair(sizeof(T), (*it).second.second)});
            } else {
                sharedVars_.insert({reinterpret_cast<size_t>(&candidate), std::make_pair(sizeof(T), cv::makePtr<std::mutex>())});
            }
        }
    }

    template<typename Tplan, typename T, bool Tcheck = true>
    bool checkShared(Tplan& plan, const T& candidate) {
        {
            std::lock_guard<std::mutex> guard(safeVarsMtx_);
            if(safeVars_.find(reinterpret_cast<size_t>(&candidate)) != safeVars_.end()) {
                return false;
            }
        }
        std::lock_guard<std::mutex> guard(sharedVarsMtx_);
        if(sharedVars_.find(reinterpret_cast<size_t>(&candidate)) != sharedVars_.end()) {
            return true;
        } else if(!isPlanMember(plan, candidate)) {
            auto parent = findSharedParent(candidate);
            if(parent.first != 0) {
                auto it = sharedVars_.find(parent.first);
                CV_Assert(it != sharedVars_.end());
                sharedVars_.insert({reinterpret_cast<size_t>(&candidate), std::make_pair(sizeof(T), (*it).second.second)});
            } else {
                sharedVars_.insert({reinterpret_cast<size_t>(&candidate), std::make_pair(sizeof(T), cv::makePtr<std::mutex>())});
            }
            return true;
        }
        return false;
    }

    template<typename T>
    void registerSafe(const T& safe) {
        std::lock_guard<std::mutex> guard(safeVarsMtx_);
        auto it = safeVars_.find(reinterpret_cast<size_t>(&safe));
        if(it == safeVars_.end()) {
            safeVars_.insert({reinterpret_cast<size_t>(&safe), std::make_pair(sizeof(T), cv::makePtr<std::mutex>())});
        }
    }

    template<typename T>
    void safe_copy(const T& from, T& to) {
        std::mutex* mtx = getMutexPtr(from, false);
        mtx = mtx == nullptr ? getMutexPtr(to, false) : mtx;
        if(mtx == nullptr)
            throw std::runtime_error("Internal error: Trying to safe copy non-shared variables.");
        std::lock_guard<std::mutex> guard(*mtx);
        to = copy(from);
    }

    template<typename T>
    static void copy(const T& from, T& to) {
        to = copy_construct(from);
    }

    static void copy(const cv::UMat& from, cv::UMat& to) {
        if(from.empty())
            return;
        to = from.clone();
    }

    template<typename T>
    static T safe_copy(const T& from) {
        T to;
        safe_copy(from, to);
        return to;
    }

    template<typename T>
    static T copy(const T& from) {
        T to;
        copy(from, to);
        return to;
    }

    template<typename T>
    static T copy_construct(const T& t) {
        return t;
    }

    template<typename T>
    std::mutex* getMutexPtr(const T& shared, bool check = true) {
        SharedVarsIter it, end;
        cv::Ptr<std::mutex> mtx = nullptr;
        std::lock_guard<std::mutex> guard(sharedVarsMtx_);
        it = sharedVars_.find(reinterpret_cast<size_t>(&shared));
        end = sharedVars_.end();
        if(it != end) {
            mtx = (*it).second.second;
        }
        if(check && !mtx)
            throw std::runtime_error("You are trying to lock a non-shared variable");
        return mtx.get();
    }

    template<typename T>
    void lock(const T& shared) {
        getMutexPtr(shared)->lock();
    }

    template<typename T>
    void unlock(const T& shared) {
        getMutexPtr(shared)->unlock();
    }

    template<typename T>
    bool tryLock(const T& shared) {
        return getMutexPtr(shared)->try_lock();
    }
};

class CV_EXPORTS GlobalState {
public:
    struct Keys {
        enum Enum {
            FRAME_CNT,
            RUN_CNT,
            START_TIME,
            FPS,
            WORKERS_READY,
            WORKERS_STARTED,
            LOCKING,
            LOCK_CONTENTION_CNT,
            LOCK_CONTENTION_RATE,
            LCR_CNT,
            TIME_TRACKER
        };
    };

private:
    CV_EXPORTS static ThreadSafeAnyMap<Keys::Enum> map_;
    CV_EXPORTS static std::mutex threadIDMtx_;
    CV_EXPORTS static const std::thread::id defaultThreadID_;
    CV_EXPORTS static std::thread::id mainThreadID_;
    CV_EXPORTS static bool isFirstRun_;
    CV_EXPORTS static std::set<string> once_;
    CV_EXPORTS static std::mutex nodeLockMtx_;
    CV_EXPORTS static std::map<string, std::pair<std::thread::id, cv::Ptr<std::mutex>>> nodeLockMap_;
    CV_EXPORTS static SharedVariables sharedVars_;

    CV_EXPORTS static cv::Ptr<std::mutex> getNodeLockInternal(const string& name, const bool owned = true);
    CV_EXPORTS static bool invalidateNodeLockInternal(const string& name);

public:
    CV_EXPORTS static void init_keys();
    CV_EXPORTS static SharedVariables& shared_vars();

    template <typename V>
    static const auto& get(Keys::Enum k) {
        return map_.get<V>(k);
    }

    template <typename V>
    static void set(Keys::Enum k, V v) {
        map_.set(k, v);
    }

    template <bool Tread, typename V>
    static void create(Keys::Enum k, V v, const std::function<void(const V& val)>& cb = std::function<void(const V& val)>()) {
        map_.create<Tread>(k, v, cb);
    }

    template <typename V>
    static V apply(Keys::Enum k, std::function<V(V&)> f) {
        return map_.apply(k, f);
    }

    CV_EXPORTS static void setMainID(const std::thread::id& id);
    CV_EXPORTS static bool isMain();
    CV_EXPORTS static bool isFirstRun();
    CV_EXPORTS static cv::Ptr<std::mutex> tryGetNodeLock(const string& name);
    CV_EXPORTS static bool lockNode(const string& name);
    CV_EXPORTS static bool tryUnlockNode(const string& name);
    CV_EXPORTS static size_t countNodeLocks();
    CV_EXPORTS static bool once(string name);
};

class CV_EXPORTS LocalState {
public:
    struct Keys {
        enum Enum {
            WORKER_INDEX,
        };
    };

private:
    CV_EXPORTS static thread_local ThreadSafeAnyMap<Keys::Enum> map_;

public:
    static void init_keys() {
        create<false, size_t>(Keys::WORKER_INDEX, 0);
    }

    template <typename V>
    static const V& get(Keys::Enum k) {
        return map_.get<V>(k);
    }

    template <typename V>
    static void set(Keys::Enum k, V v) {
        map_.set(k, v);
    }

    template <bool Tread, typename V>
    static void create(Keys::Enum k, V v, const std::function<void(const V& val)>& cb = std::function<void(const V& val)>()) {
        map_.create<Tread>(k, v, cb);
    }

    template <typename V>
    static V apply(Keys::Enum k, std::function<V(V&)> f) {
        return map_.apply(k, f);
    }
};

CV_EXPORTS void setThreadName(const char* threadName);

inline double seconds() {
    return cv::getTickCount() / cv::getTickFrequency();
}

} // namespace plan
} // namespace cv

#endif // OPENCV_PLAN_UTIL_HPP_

