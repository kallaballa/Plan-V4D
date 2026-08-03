// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
#ifndef OPENCV_PLAN_RUNTIME_HPP_
#define OPENCV_PLAN_RUNTIME_HPP_

#include "context.hpp"
#include "anymap.hpp"
#include "util.hpp"
#include <map>
#include <string>
#include <mutex>

namespace plan {

class Plan;

class PLAN_EXPORTS Runtime {
    friend class Plan;

public:
    struct Keys {
        enum Enum { SIZE, VIEWPORT, CLEAR_COLOR, NAMESPACE, NUM_KEYS };
    };

private:
    PLAN_EXPORTS static std::mutex instance_mtx_;
    PLAN_EXPORTS static thread_local Ptr<Runtime> instance_;
    PLAN_EXPORTS static thread_local ThreadSafeAnyMap<Keys::Enum> properties_;

    std::map<std::string, Ptr<Context>> contexts_;
    Ptr<PlainContext> plainContext_;
    std::mutex ctxMtx_;
    Size size_;
    Rect viewport_;
    bool running_ = true;

public:
    PLAN_EXPORTS static Ptr<Runtime> instance();
    PLAN_EXPORTS static Ptr<Runtime> init(const Size& sz, const string& name = "plan");
    PLAN_EXPORTS virtual ~Runtime();

    static void init_keys();

    template<bool Tread, typename Tval>
    static void create(Keys::Enum key, const Tval& val,
        const std::function<void(const Tval& val)>& cb =
            std::function<void(const Tval& val)>()) {
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

    PLAN_EXPORTS void registerContext(const std::string& name, Ptr<Context> ctx);
    PLAN_EXPORTS Ptr<Context> getContext(const std::string& name);
    PLAN_EXPORTS bool hasContext(const std::string& name);
    PLAN_EXPORTS Ptr<PlainContext> plainCtx();
    PLAN_EXPORTS Rect viewport() const;
    PLAN_EXPORTS void setViewport(const Rect& vp);
    PLAN_EXPORTS Size size() const;
    PLAN_EXPORTS void stop();
    PLAN_EXPORTS bool isRunning() const;
    PLAN_EXPORTS static void run(Ptr<Runtime> runtime, std::function<void()> runGraph);

private:
    Runtime(const Size& sz, const string& name);
};

} // namespace plan

#endif // OPENCV_PLAN_RUNTIME_HPP_

