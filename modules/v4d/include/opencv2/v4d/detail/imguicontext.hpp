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
struct GLFWwindow;
namespace cv {
namespace v4d {
namespace detail {

class CV_EXPORTS ImGuiContextImpl : public cv::plan::detail::PlanContext {
    friend class cv::v4d::V4D;
    cv::Ptr<FrameBufferContext> mainFbContext_;
    // The ImGui context of the window this instance draws.
    ImGuiContext* ctx_ = nullptr;
    // "Current" ImGui context of the calling thread. ImGui's GImGui and
    // ImGui::Get/SetCurrentContext() are redirected here (see imconfig.h), so
    // making this a thread-local gives every thread - i.e. every plan that
    // displays a window - its own current context for free.
    inline static thread_local ImGuiContext* context_;
    cv::Ptr<cv::plan::Transaction> renderCallback_;
    bool firstFrame_ = true;
public:
    CV_EXPORTS ImGuiContextImpl(cv::Ptr<FrameBufferContext> fbContext);
    CV_EXPORTS ~ImGuiContextImpl();
    CV_EXPORTS void setTransaction(cv::Ptr<cv::plan::Transaction> tx);
    // The context this instance draws into.
    CV_EXPORTS ImGuiContext* context() const {
        return ctx_;
    }
    /*!
     * Makes this window's context the current context of the calling thread, so
     * that the ImGui calls which follow (backend callbacks, drawing) operate on
     * this window's GUI. Needed whenever a thread touches the GUI of a window it
     * does not display, e.g. in a GLFW input callback.
     */
    CV_EXPORTS void makeCurrent();

    // Current context of the calling thread. Used by ImGui itself (GImGui).
    CV_EXPORTS static ImGuiContext* getContext();
    CV_EXPORTS static void setContext(ImGuiContext* ctx);

    /*!
     * Forwards a GLFW input event to the ImGui GLFW backend with this window's
     * context made current, and reports whether ImGui wants to capture the
     * input. The event is ignored if this runtime has no GUI.
     */
    CV_EXPORTS bool forwardKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    CV_EXPORTS bool forwardMouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    CV_EXPORTS bool forwardScrollCallback(GLFWwindow* window, double xoffset, double yoffset);
    CV_EXPORTS bool forwardCursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    CV_EXPORTS bool forwardCharCallback(GLFWwindow* window, unsigned int codepoint);
protected:
    CV_EXPORTS int execute(const cv::Rect& vp, std::function<void()> fn) override;
};
}
}
}

#endif /* SRC_OPENCV_IMGUIContext_HPP_ */
