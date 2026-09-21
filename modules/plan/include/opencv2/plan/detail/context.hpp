// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include <functional>
#include <opencv2/core/types.hpp>

#ifndef OPENCV_PLAN_DETAIL_CONTEXT_HPP_
#define OPENCV_PLAN_DETAIL_CONTEXT_HPP_

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

}
}
}

#endif /* OPENCV_PLAN_DETAIL_CONTEXT_HPP_ */
