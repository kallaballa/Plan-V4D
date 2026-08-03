// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
#ifndef OPENCV_PLAN_CONTEXT_HPP_
#define OPENCV_PLAN_CONTEXT_HPP_

#include "defs.hpp"
#include <functional>

namespace plan {

class PLAN_EXPORTS Context {
public:
    virtual ~Context() {}
    virtual int execute(const Rect& vp, std::function<void()> fn) = 0;
};

class PLAN_EXPORTS PlainContext : public Context {
public:
    virtual ~PlainContext() {}
    virtual int execute(const Rect& vp, std::function<void()> fn) override {
        PLAN_UNUSED(vp);
        fn();
        return 1;
    }
};

} // namespace plan

#endif // OPENCV_PLAN_CONTEXT_HPP_

