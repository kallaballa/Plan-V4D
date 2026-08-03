// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
#ifndef OPENCV_PLAN_DEFS_HPP_
#define OPENCV_PLAN_DEFS_HPP_

#include <memory>
#include <stdexcept>
#include <string>
#include <sstream>
#include <cassert>
#include <cstdint>
#include <typeinfo>
#include <cstring>

#ifdef __GNUG__
#include <cstdlib>
#include <cxxabi.h>
#endif

// ── Export / visibility ──────────────────────────────────────────────
#if defined(_WIN32)
#  if defined(plan_EXPORTS)
#    define PLAN_EXPORTS __declspec(dllexport)
#  else
#    define PLAN_EXPORTS __declspec(dllimport)
#  endif
#else
#  define PLAN_EXPORTS __attribute__((visibility("default")))
#endif

// ── Small utility macros ─────────────────────────────────────────────
#define PLAN_UNUSED(x) (void)(x)

#define PLAN_Assert(expr)                                              \
    do {                                                               \
        if (!(expr)) {                                                 \
            throw std::runtime_error(                                  \
                std::string("Assertion failed: ") + #expr +            \
                " at " + __FILE__ + ":" + std::to_string(__LINE__));   \
        }                                                              \
    } while (0)

#define PLAN_Error(msg)                                                \
    throw std::runtime_error(msg)

// ── Namespace-scope aliases ──────────────────────────────────────────
namespace plan {

//! Shared-owning smart pointer (replaces cv::Ptr).
template<typename T>
using Ptr = std::shared_ptr<T>;

//! Factory helper (replaces cv::makePtr).
template<typename T, typename... Args>
Ptr<T> makePtr(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
}

//! Create a non-owning Ptr that points to `p` but will never delete it.
template<typename T>
Ptr<T> nonOwning(T* p) {
    return Ptr<T>(p, [](T*){});
}

// ── Lightweight value types (replace cv::Size / cv::Rect / cv::Scalar) ──

struct Size {
    int width  = 0;
    int height = 0;
    Size() = default;
    Size(int w, int h) : width(w), height(h) {}
    bool operator==(const Size& o) const { return width == o.width && height == o.height; }
    bool operator!=(const Size& o) const { return !(*this == o); }
};

struct Rect {
    int x = 0, y = 0, width = 0, height = 0;
    Rect() = default;
    Rect(int x_, int y_, int w_, int h_) : x(x_), y(y_), width(w_), height(h_) {}
    Size size() const { return {width, height}; }
    bool operator==(const Rect& o) const {
        return x == o.x && y == o.y && width == o.width && height == o.height;
    }
    bool operator!=(const Rect& o) const { return !(*this == o); }
};

struct Scalar {
    double val[4] = {0, 0, 0, 0};
    Scalar() = default;
    Scalar(double v0, double v1 = 0, double v2 = 0, double v3 = 0) {
        val[0] = v0; val[1] = v1; val[2] = v2; val[3] = v3;
    }
    static Scalar all(double v) { return {v, v, v, v}; }
    bool operator==(const Scalar& o) const { return std::memcmp(val, o.val, sizeof(val)) == 0; }
    bool operator!=(const Scalar& o) const { return !(*this == o); }
};

// ── Demangle helper ──────────────────────────────────────────────────
namespace detail {
#ifdef __GNUG__
inline std::string demangle(const char* name) {
    int status = -4;
    std::unique_ptr<char, void(*)(void*)> res{
        abi::__cxa_demangle(name, nullptr, nullptr, &status), std::free};
    return (status == 0) ? res.get() : name;
}
#else
inline std::string demangle(const char* name) { return name; }
#endif

template<typename T>
std::string type_name() { return demangle(typeid(T).name()); }
} // namespace detail

} // namespace plan

#endif // OPENCV_PLAN_DEFS_HPP_

