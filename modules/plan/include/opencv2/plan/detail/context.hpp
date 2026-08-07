// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
#ifndef OPENCV_PLAN_DETAIL_CONTEXT_HPP_
#define OPENCV_PLAN_DETAIL_CONTEXT_HPP_

#include <functional>
#include <opencv2/core/types.hpp>
#include <opencv2/core/cvdef.h>

namespace cv {
namespace plan {
namespace detail {

/*!
 * Base class of all execution contexts used by Plan transactions.
 */
class PlanContext {
public:
    virtual ~PlanContext() {}
    virtual int execute(const cv::Rect& vp, std::function<void()> fn) = 0;
};

/*!
 * A context with no side effects. Simply executes the transaction.
 */
class PlainContext : public PlanContext {
public:
    virtual ~PlainContext() {}
    virtual int execute(const cv::Rect& vp, std::function<void()> fn) override {
        CV_UNUSED(vp);
        fn();
        return 1;
    }
};

} /* namespace detail */
} /* namespace plan */
} /* namespace cv */
#endif /* OPENCV_PLAN_DETAIL_CONTEXT_HPP_ */
