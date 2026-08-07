// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
#ifndef MODULES_PLAN_INCLUDE_OPENCV2_PLAN_DETAIL_CONTEXT_HPP_
#define MODULES_PLAN_INCLUDE_OPENCV2_PLAN_DETAIL_CONTEXT_HPP_
#include <functional>
#include "../base.hpp"
namespace cv {
namespace plan {
namespace detail {
class PlanContext {
public:
    virtual ~PlanContext() {}
    virtual int execute(const Rect& vp, std::function<void()> fn) = 0;
};
class PlainContext : public PlanContext {
public:
    virtual ~PlainContext() {}
    virtual int execute(const Rect& vp, std::function<void()> fn) override {
        PLAN_UNUSED(vp);
        fn();
        return 1;
    }
};
} // namespace detail
} // namespace plan
} // namespace cv
#endif /* MODULES_PLAN_INCLUDE_OPENCV2_PLAN_DETAIL_CONTEXT_HPP_ */
