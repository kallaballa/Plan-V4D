// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#ifndef SRC_OPENCV_IMGUIContext_HPP_
#define SRC_OPENCV_IMGUIContext_HPP_

#if defined(OPENCV_V4D_USE_ES3)
#   define IMGUI_IMPL_OPENGL_ES3
#endif


#include "framebuffercontext.hpp"
#include <opencv2/plan/detail/transaction.hpp>
#include "imgui.h"

struct ImGuiContext;
namespace cv {
namespace v4d {
namespace detail {

class CV_EXPORTS ImGuiContextImpl : public cv::plan::detail::PlanContext {
    friend class cv::v4d::V4D;
    cv::Ptr<FrameBufferContext> mainFbContext_;
    inline static ImGuiContext* context_;
    cv::Ptr<cv::plan::Transaction> renderCallback_;
    bool firstFrame_ = true;
public:
    CV_EXPORTS ImGuiContextImpl(cv::Ptr<FrameBufferContext> fbContext);
    CV_EXPORTS void setTransaction(cv::Ptr<cv::plan::Transaction> tx);
    CV_EXPORTS static ImGuiContext* getContext();
    CV_EXPORTS static void setContext(ImGuiContext* ctx);
protected:
    CV_EXPORTS int execute(const cv::Rect& vp, std::function<void()> fn) override;
};
}
}
}

#endif /* SRC_OPENCV_IMGUIContext_HPP_ */
