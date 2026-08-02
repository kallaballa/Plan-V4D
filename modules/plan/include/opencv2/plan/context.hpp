#ifndef OPENCV_PLAN_CONTEXT_HPP_
#define OPENCV_PLAN_CONTEXT_HPP_

#include <functional>
#include <opencv2/core/types.hpp>

namespace cv {
namespace plan {
namespace detail {

class PlanContext {
public:
    virtual ~PlanContext() {}
    virtual int execute(const cv::Rect& vp, std::function<void()> fn) = 0;
};

class PlainContext : public PlanContext {
public:
    virtual ~PlainContext() {}
    virtual int execute(const cv::Rect& vp, std::function<void()> fn) override {
        CV_UNUSED(vp);
        fn();
        return 1;
    }
};

} // namespace detail
} // namespace plan
} // namespace cv

#endif // OPENCV_PLAN_CONTEXT_HPP_

