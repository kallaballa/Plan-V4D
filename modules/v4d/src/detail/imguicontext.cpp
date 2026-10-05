// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include <algorithm>
#include <opencv2/v4d/detail/imguicontext.hpp>
#include <opencv2/v4d/v4d.hpp>
#include <vector>

#if defined(OPENCV_V4D_USE_ES3)
#define IMGUI_IMPL_OPENGL_ES3
#endif

#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#include "imgui.h"
#if defined(__ANDROID__)
#include "imgui_impl_android.h"
// third/glfw-android is already on the include path; spelling the directory out
// again only works if modules/v4d itself happens to be on it too.
#include "glfw_android.hpp"
#else
#include "imgui_impl_glfw.h"
#endif
#include "imgui_impl_opengl3.h"

namespace cv {
namespace v4d {
namespace detail {

#if defined(__ANDROID__)
namespace {

/// Routes one AInputEvent into ImGui's Android backend.
///
/// The GLFW shim owns the AInputQueue and turns events into GLFW callbacks, but
/// ImGui's Android backend wants the events themselves -- it knows about touch
/// vs mouse vs stylus and about the on-screen keyboard, none of which survives
/// the trip through a GLFWwindow*. The shim calls this before it fires its own
/// callbacks, so the input still reaches the plan's Property events as well as
/// the GUI.
int32_t androidImGuiInput(const AInputEvent *event) {
  if (ImGui::GetCurrentContext())
    return ImGui_ImplAndroid_HandleInputEvent(event);
  return 0;
}

/// The surface ImGui's Android backend was given. It caches the ANativeWindow*,
/// so a surface that came and went (a rotation that recreates it, a trip
/// through onStop/onStart) has to be re-initialised.
ANativeWindow *gImGuiWindow = nullptr;
uint64_t gImGuiWindowGeneration = 0;

void ensureAndroidBackendWindow() {
  const uint64_t generation = glfw_android_native_window_generation();
  if (generation == gImGuiWindowGeneration)
    return;
  ANativeWindow *window = glfw_android_acquire_native_window();
  if (window == gImGuiWindow) {
    if (window)
      ANativeWindow_release(window);
    return;
  }
  if (gImGuiWindow) {
    ImGui_ImplAndroid_Shutdown();
    ANativeWindow_release(gImGuiWindow);
    gImGuiWindow = nullptr;
  }
  if (window) {
    ImGui_ImplAndroid_Init(window);
    gImGuiWindow = window;
  }
  gImGuiWindowGeneration = generation;
}

} // namespace
#endif

ImGuiContextImpl::ImGuiContextImpl(cv::Ptr<FrameBufferContext> fbContext)
    : mainFbContext_(fbContext) {
  IMGUI_CHECKVERSION();
  // ImGui::CreateContext() makes the new context current, which would break a
  // window that is already set up, so the previous current context is restored
  // when this one is initialized.
  ImGuiContext *prevCtx = ImGui::GetCurrentContext();
  ctx_ = ImGui::CreateContext();

  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  // Android has no mouse cursor to shape, and asking ImGui to try is what makes
  // it draw one at a wrong position.
#if defined(__ANDROID__)
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
#endif
  //	io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
  //	io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;
  //	io.BackendUsingLegacyNavInputArray = false;
  ImGui::StyleColorsDark();

#if defined(__ANDROID__)
  // ImGui's Android backend is handed the surface, not a window handle, and it
  // has to be re-handed whenever the activity publishes a new one.
  gImGuiWindowGeneration = 0;
  ensureAndroidBackendWindow();
  // ... and it has to be fed the raw input events, which the shim does not know
  // how to deliver on its own.
  glfw_android_set_input_handler(&androidImGuiInput);
#else
  ImGui_ImplGlfw_InitForOpenGL(mainFbContext_->getGLFWWindow(), false);
//	ImGui_ImplGlfw_SetCallbacksChainForAllWindows(true);
#endif
#if !defined(OPENCV_V4D_USE_ES3)
  ImGui_ImplOpenGL3_Init("#version 330");
#else
  ImGui_ImplOpenGL3_Init("#version 300 es");
#endif
  // Creating the context made it current; hand the current context back to
  // whoever had it, so that a second window does not steal it.
  ImGui::SetCurrentContext(prevCtx);
}

ImGuiContextImpl::~ImGuiContextImpl() {
#if defined(__ANDROID__)
  glfw_android_set_input_handler(nullptr);
  if (gImGuiWindow) {
    ImGui_ImplAndroid_Shutdown();
    ANativeWindow_release(gImGuiWindow);
    gImGuiWindow = nullptr;
    gImGuiWindowGeneration = 0;
  }
#endif
  if (ImGui::GetCurrentContext() == ctx_)
    ImGui::SetCurrentContext(nullptr);
  ImGui::DestroyContext(ctx_);
  ctx_ = nullptr;
}

ImGuiContext *ImGuiContextImpl::getContext() { return context_; }

void ImGuiContextImpl::setContext(ImGuiContext *ctx) { context_ = ctx; }

void ImGuiContextImpl::makeCurrent() {
  if (ctx_ && ImGui::GetCurrentContext() != ctx_)
    ImGui::SetCurrentContext(ctx_);
}

#if !defined(__ANDROID__)
bool ImGuiContextImpl::forwardKeyCallback(GLFWwindow *window, int key,
                                          int scancode, int action, int mods) {
  if (!ctx_)
    return false;
  makeCurrent();
  ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);
  return ImGui::GetCurrentContext() ? ImGui::GetIO().WantCaptureKeyboard
                                    : false;
}

bool ImGuiContextImpl::forwardMouseButtonCallback(GLFWwindow *window,
                                                  int button, int action,
                                                  int mods) {
  if (!ctx_)
    return false;
  makeCurrent();
  ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);
  return ImGui::GetCurrentContext() ? ImGui::GetIO().WantCaptureMouse : false;
}

bool ImGuiContextImpl::forwardScrollCallback(GLFWwindow *window, double xoffset,
                                             double yoffset) {
  if (!ctx_)
    return false;
  makeCurrent();
  ImGui_ImplGlfw_ScrollCallback(window, xoffset, yoffset);
  return ImGui::GetCurrentContext() ? ImGui::GetIO().WantCaptureMouse : false;
}

bool ImGuiContextImpl::forwardCursorPosCallback(GLFWwindow *window, double xpos,
                                                double ypos) {
  if (!ctx_)
    return false;
  makeCurrent();
  ImGui_ImplGlfw_CursorPosCallback(window, xpos, ypos);
  return ImGui::GetCurrentContext() ? ImGui::GetIO().WantCaptureMouse : false;
}

bool ImGuiContextImpl::forwardCharCallback(GLFWwindow *window,
                                           unsigned int codepoint) {
  if (!ctx_)
    return false;
  makeCurrent();
  ImGui_ImplGlfw_CharCallback(window, codepoint);
  return ImGui::GetCurrentContext() ? ImGui::GetIO().WantCaptureKeyboard
                                    : false;
}
#else
// On Android the GUI is fed straight from the AInputEvents (see
// androidImGuiInput), so the GLFW callback forwarders have nothing to do. They
// still exist, and still answer, because FrameBufferContext's GLFW callbacks
// fire for plan input events as well as for the GUI, and a plan that wants to
// know whether the GUI ate the input is asking a real question.
#define V4D_IMGUI_NO_FORWARD(fn, capture)                                      \
  bool ImGuiContextImpl::fn {                                                  \
    return ctx_ && ImGui::GetCurrentContext() ? ImGui::GetIO().capture         \
                                              : false;                         \
  }
V4D_IMGUI_NO_FORWARD(forwardKeyCallback(GLFWwindow *, int, int, int, int),
                     WantCaptureKeyboard)
V4D_IMGUI_NO_FORWARD(forwardMouseButtonCallback(GLFWwindow *, int, int, int),
                     WantCaptureMouse)
V4D_IMGUI_NO_FORWARD(forwardScrollCallback(GLFWwindow *, double, double),
                     WantCaptureMouse)
V4D_IMGUI_NO_FORWARD(forwardCursorPosCallback(GLFWwindow *, double, double),
                     WantCaptureMouse)
V4D_IMGUI_NO_FORWARD(forwardCharCallback(GLFWwindow *, unsigned int),
                     WantCaptureKeyboard)
#undef V4D_IMGUI_NO_FORWARD
#endif

void ImGuiContextImpl::setTransaction(cv::Ptr<Transaction> tx) {
  renderCallback_ = tx;
}

int ImGuiContextImpl::execute(const cv::Rect &vp, std::function<void()> fn) {
  CV_UNUSED(fn);
  CV_UNUSED(vp);
  if (ctx_ && GlobalState::get<bool>(GlobalState::Keys::SHOW_GUI)) {
    // The GUI of this window is drawn from its display thread, so it must be
    // the current context even if another plan's context is more recent.
    makeCurrent();
    ImGui_ImplOpenGL3_NewFrame();
#if defined(__ANDROID__)
    // A surface that arrived or went away since the last frame changes what the
    // backend is bound to.
    ensureAndroidBackendWindow();
    ImGui_ImplAndroid_NewFrame();
#else
    ImGui_ImplGlfw_NewFrame();
#endif
    ImGui::NewFrame();

    bool open_ptr[1] = {true};
    ImGuiWindowFlags window_flags = 0;
    //            window_flags |= ImGuiWindowFlags_NoBackground;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
    window_flags |= ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoScrollWithMouse;
    window_flags |= ImGuiWindowFlags_AlwaysAutoResize;
    window_flags |= ImGuiWindowFlags_NoSavedSettings;
    window_flags |= ImGuiWindowFlags_NoFocusOnAppearing;
    window_flags |= ImGuiWindowFlags_NoNav;
    window_flags |= ImGuiWindowFlags_NoDecoration;
    window_flags |= ImGuiWindowFlags_NoInputs;
    if (GlobalState::get<bool>(GlobalState::Keys::SHOW_FRAME_TIME)) {
      ImGuiStyle &style = ImGui::GetStyle();
      style.FontScaleDpi = 1.6f;
      ImVec2 pos(0, 0);
      ImGui::SetNextWindowPos(pos, ImGuiCond_Once);
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));
      ImGui::Begin("Display", open_ptr, window_flags);
      double fps = GlobalState::get<double>(GlobalState::Keys::FPS);
      size_t workers =
          GlobalState::get<size_t>(GlobalState::Keys::WORKERS_READY);
      ImGui::Text("%.4f ms/frame (%.1f FPS), workers: %ld", (1000.0f / fps),
                  fps, workers);
      ImGui::End();
      ImGui::PopStyleColor(1);
    }
    if (GlobalState::get<bool>(GlobalState::Keys::TIME_TRACKER)) {
      // The widget is the view of the TIME_TRACKER property: closing it
      // clears the property, which also stops the workers from measuring
      // for a widget nobody is looking at.
      if (!timeTrackerWidget_.draw(*TimeTracker::getInstance()))
        GlobalState::set(GlobalState::Keys::TIME_TRACKER, false);
    }
    if (renderCallback_)
      renderCallback_->perform();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  }
  return 1;
}
} // namespace detail
} // namespace v4d
} // namespace cv
