// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <opencv2/v4d/detail/imguicontext.hpp>
#include <opencv2/v4d/v4d.hpp>
#include <vector>

#if defined(OPENCV_V4D_USE_ES3)
#define IMGUI_IMPL_OPENGL_ES3
#endif

#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#include "imgui.h"
// For the ImGuiContext window list and the dock-node fields: freeRect() reads
// the layout of the last completed frame to find the space a framebuffer can
// be blitted into without being covered by the GUI.
#include "imgui_internal.h"
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

/// The density last handed to the backend. -1 so the first call applies.
float gImGuiDensity = -1.0f;

/// Hands the display density to the backend, which turns it into the style,
/// the font metric and the touch padding the GUI is drawn at.
///
/// Android reports its pixel ratio in dp (one dp is a pixel at 160 dpi) and
/// everything in ImGui's default style is sized for 160 dpi: on a phone with a
/// 480 dpi display that leaves the GUI a third of the size it is meant to be,
/// with the text drawn at a third of the size too. The backend scales all of
/// it, so a window that fits a mouse pointer becomes one that fits a finger.
///
/// Cheap enough to check every frame -- a read of the shim's cached value while
/// it is unchanged -- and a display zoom switch or a change of display can
/// move it at any time, with or without a new surface.
void applyAndroidDensity() {
  const float density = glfw_android_display_density();
  if (density == gImGuiDensity)
    return;
  gImGuiDensity = density;
  ImGui_ImplAndroid_SetDensity(density);
}

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
    // The style the density scales is the one the app configured, so it goes
    // after the init, not before it.
    applyAndroidDensity();
    gImGuiWindow = window;
  }
  gImGuiWindowGeneration = generation;
}

} // namespace
#endif

namespace {

/// The largest axis-aligned rectangle inside `bounds` that does not overlap
/// any rectangle of `obstacles` (all in the same coordinate space).
///
/// A maximal empty rectangle can always be grown until each of its edges rests
/// on a bound or on an obstacle edge, so it is enough to look at the grid
/// formed by those coordinates. That grid has only two lines per obstacle, and
/// the maximal rectangle in it is the classic per-row histogram problem, which
/// a monotonic stack solves in linear time per row.
cv::Rect largestEmptyRect(const cv::Rect &bounds,
                          const std::vector<cv::Rect> &obstacles) {
  if (bounds.width <= 0 || bounds.height <= 0)
    return cv::Rect();

  std::vector<cv::Rect> clipped;
  clipped.reserve(obstacles.size());
  for (const cv::Rect &o : obstacles) {
    const cv::Rect c = o & bounds;
    if (c.width > 0 && c.height > 0)
      clipped.push_back(c);
  }
  if (clipped.empty())
    return bounds;

  std::vector<int> xs = {bounds.x, bounds.x + bounds.width};
  std::vector<int> ys = {bounds.y, bounds.y + bounds.height};
  for (const cv::Rect &o : clipped) {
    xs.push_back(o.x);
    xs.push_back(o.x + o.width);
    ys.push_back(o.y);
    ys.push_back(o.y + o.height);
  }
  std::sort(xs.begin(), xs.end());
  xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
  std::sort(ys.begin(), ys.end());
  ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

  const int nx = static_cast<int>(xs.size()) - 1; // grid cells per row
  const int ny = static_cast<int>(ys.size()) - 1;
  if (nx <= 0 || ny <= 0)
    return cv::Rect();

  // Mark every grid cell an obstacle covers. Its edges are grid lines, so
  // lower_bound gives the exact half-open cell range.
  std::vector<unsigned char> freeCell(static_cast<size_t>(nx) * ny, 1);
  for (const cv::Rect &o : clipped) {
    const int i0 = static_cast<int>(
        std::lower_bound(xs.begin(), xs.end(), o.x) - xs.begin());
    const int i1 = static_cast<int>(
        std::lower_bound(xs.begin(), xs.end(), o.x + o.width) - xs.begin());
    const int j0 = static_cast<int>(
        std::lower_bound(ys.begin(), ys.end(), o.y) - ys.begin());
    const int j1 = static_cast<int>(
        std::lower_bound(ys.begin(), ys.end(), o.y + o.height) - ys.begin());
    for (int j = j0; j < j1; ++j)
      for (int i = i0; i < i1; ++i)
        freeCell[static_cast<size_t>(i) + static_cast<size_t>(j) * nx] = 0;
  }

  // Sweep the rows bottom-up, carrying the height (in pixels) of the free run
  // that ends at the current row for every column, and resolve each row's
  // weighted histogram with a monotonic stack. The widths are real grid
  // spacings, so the area is in pixels and the winning rectangle is exact.
  cv::Rect best;
  long long bestArea = 0;
  std::vector<int> height(nx, 0);
  struct Bar {
    int start;  // first grid column of the run
    int height; // run height in pixels
  };
  std::vector<Bar> stack;
  stack.reserve(nx);
  for (int j = 0; j < ny; ++j) {
    for (int i = 0; i < nx; ++i) {
      height[i] =
          freeCell[static_cast<size_t>(i) + static_cast<size_t>(j) * nx]
              ? height[i] + (ys[j + 1] - ys[j])
              : 0;
    }
    stack.clear();
    for (int i = 0; i <= nx; ++i) {
      const int h = (i < nx) ? height[i] : 0; // 0 flushes the stack
      int start = i;
      while (!stack.empty() && stack.back().height > h) {
        const Bar bar = stack.back();
        stack.pop_back();
        const int width = xs[i] - xs[bar.start];
        const long long area = static_cast<long long>(bar.height) * width;
        if (area > bestArea) {
          bestArea = area;
          best = cv::Rect(xs[bar.start], ys[j + 1] - bar.height, width,
                          bar.height);
        }
        start = bar.start;
      }
      if (stack.empty() || stack.back().height < h)
        stack.push_back({start, h});
    }
  }
  return best;
}

} // namespace

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
  // Native docking: plans get a dockable panel layout. Multi-viewport is left
  // off on purpose -- V4D draws ImGui into its own framebuffer and never calls
  // UpdatePlatformWindows(), so the dockspace must stay inside the main viewport.
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
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
  ImGui_ImplOpenGL3_Init(
#if defined(OPENCV_V4D_USE_ES3)
      // The backend picks its shader variants by parsing the version number
      // out of this string, and only "== 300" reaches the GLES ones.
      // OPENCV_V4D_GL_SHADER_VERSION is "#version 320 es", which parses as
      // 320, falls through to the desktop GLSL 130 shaders and fails to
      // compile on a GLES context (the fragment stage has no default float
      // precision) - leaving ShaderHandle 0 and every frame's draw dropped.
      // The 300 es shaders compile fine on ES 3.x, 320 included.
      "#version 300 es\n"
#else
      (std::string(OPENCV_V4D_GL_SHADER_VERSION) + "\n").c_str()
#endif
  );
  // Creating the context made it current; hand the current context back to
  // whoever had it, so that a second window does not steal it.
  ImGui::SetCurrentContext(prevCtx);
  backendInitialized_ = true;
}

ImGuiContextImpl::~ImGuiContextImpl() {
#if defined(__ANDROID__)
  glfw_android_set_input_handler(nullptr);
  if (gImGuiWindow) {
    ANativeWindow_release(gImGuiWindow);
    gImGuiWindow = nullptr;
    gImGuiWindowGeneration = 0;
  }
#endif
  if (backendInitialized_ && ctx_) {
    // Shut the backends down before the context is destroyed. ImGui_ImplOpenGL3
    // owns a font-atlas texture, its shader and its vertex/index buffers, and
    // ImGui_ImplGlfw owns the GLFW callbacks it installed; without these calls
    // every runtime leaked its GPU objects and left the window's callbacks
    // pointing at a context that no longer exists. Both need their ImGui
    // context current and the GL context current.
    ImGui::SetCurrentContext(ctx_);
    FrameBufferContext::WindowScope winScope(mainFbContext_);
    FrameBufferContext::GLScope glScope(mainFbContext_, GL_FRAMEBUFFER);
    ImGui_ImplOpenGL3_Shutdown();
#if defined(__ANDROID__)
    ImGui_ImplAndroid_Shutdown();
#else
    ImGui_ImplGlfw_Shutdown();
#endif
  }
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

cv::Rect ImGuiContextImpl::freeRect(const cv::Size &fbSize) {
  const cv::Rect bounds(0, 0, std::max(fbSize.width, 0),
                        std::max(fbSize.height, 0));
  if (bounds.width <= 0 || bounds.height <= 0)
    return cv::Rect();
  // Before any frame has run there is no layout to ask about.
  if (!ctx_)
    return bounds;

  makeCurrent();
  ImGuiContext *context = ImGui::GetCurrentContext();
  if (context == nullptr)
    return bounds;

  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  const ImVec2 origin = viewport ? viewport->Pos : ImVec2(0.0f, 0.0f);
  const ImGuiIO &io = ImGui::GetIO();
  const float scaleX =
      io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
  const float scaleY =
      io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;

  std::vector<cv::Rect> obstacles;
  const bool dbg = std::getenv("V4D_FREERECT_DEBUG") != nullptr;
  for (ImGuiWindow *window : context->Windows) {
    if (window == nullptr)
      continue;
    if (dbg)
      std::fprintf(
          stderr,
          "[freerect] win='%s' pos=(%.0f,%.0f) size=(%.0f,%.0f) hidden=%d "
          "active=%d was=%d fallback=%d child=%d dockhost=%d nobg=%d\n",
          window->Name, window->Pos.x, window->Pos.y, window->Size.x,
          window->Size.y, (int)window->Hidden, (int)window->Active,
          (int)window->WasActive, (int)window->IsFallbackWindow,
          (int)((window->Flags & ImGuiWindowFlags_ChildWindow) != 0),
          (int)(window->DockNodeAsHost != nullptr),
          (int)((window->Flags & ImGuiWindowFlags_NoBackground) != 0));
    if (window->Hidden || window->IsFallbackWindow)
      continue;
    // Active is set during Begin() and cleared again at the end of the same
    // ImGui frame (imgui.cpp EndFrame: "WasActive = Active; Active = false;").
    // freeRect() runs between frames, before the next NewFrame, so it is
    // WasActive - "was submitted in the frame we are looking at" - that says
    // whether the window was really part of the layout; Active alone would
    // discard every window and report the whole window as free.
    if (!window->Active && !window->WasActive)
      continue;
    // A window with no background paints nothing over the framebuffer. A
    // dockspace host is exactly that, and it spans the whole work area, so
    // counting it would report the window as entirely covered and hide the
    // very hole the layout leaves for content like the framebuffer. Docked
    // panels do have a background and must be counted: they are child windows
    // of the host, which is why child windows are not skipped wholesale here.
    // The children *inside* a panel are covered by their parent, so counting
    // them as well changes nothing.
    if ((window->Flags & ImGuiWindowFlags_NoBackground) != 0)
      continue;
    if (window->Size.x < 1.0f || window->Size.y < 1.0f)
      continue;
    obstacles.emplace_back(
        static_cast<int>(std::lround((window->Pos.x - origin.x) * scaleX)),
        static_cast<int>(std::lround((window->Pos.y - origin.y) * scaleY)),
        static_cast<int>(std::lround(window->Size.x * scaleX)),
        static_cast<int>(std::lround(window->Size.y * scaleY)));
  }
  const cv::Rect free = largestEmptyRect(bounds, obstacles);
  if (dbg)
    std::fprintf(stderr,
                 "[freerect] bounds=(%d,%d %dx%d) obstacles=%zu -> (%d,%d "
                 "%dx%d)\n",
                 bounds.x, bounds.y, bounds.width, bounds.height,
                 obstacles.size(), free.x, free.y, free.width, free.height);
  return free;
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
    // backend is bound to, and a display whose density changed -- a zoom switch,
    // another display -- changes the scale the GUI is drawn at. The density
    // call is a cached read unless it changed.
    ensureAndroidBackendWindow();
    applyAndroidDensity();
    ImGui_ImplAndroid_NewFrame();
    // The input method delivers characters through the activity's Java layer
    // (the NDK has no AKeyEvent_getUnicodeChar()), so drain the queue that the
    // shim polls into, before NewFrame() hands the characters to this frame's
    // widgets. Gated on the previous frame wanting text input, so an idle GUI
    // pays no JNI cost.
    if (lastWantTextInput_) {
      ImGuiIO &io = ImGui::GetIO();
      for (int32_t c = glfw_android_poll_unicode_char(); c != 0;
           c = glfw_android_poll_unicode_char())
        io.AddInputCharacter(static_cast<unsigned int>(c));
    }
#else
    ImGui_ImplGlfw_NewFrame();
#endif
    ImGui::NewFrame();
    ImGuiStyle &style = ImGui::GetStyle();
    style.FontScaleDpi = 2.0;
#if defined(__ANDROID__)
    // Mobile has no physical keyboard to fall back on: io.WantTextInput is the
    // signal ImGui gives for "a text field is focused", so the on-screen
    // keyboard follows it. NewFrame() refreshes the flag from the previous
    // frame's widget state, hence the one-frame latency.
    //
    // Touching a window scrollbar focuses that window as a side effect
    // (ButtonBehavior -> FocusWindow), which a text editor inside it reports as
    // "wants text input" -- so scrolling the code pane used to raise the
    // keyboard. Pressing a scrollbar must therefore not raise a hidden
    // keyboard; any other press re-enables it.
    {
      ImGuiIO &io = ImGui::GetIO();
      bool show = io.WantTextInput;
      if (show && lastPressWasScrollbar_ && !lastWantTextInput_)
        show = false;
      if (show != lastWantTextInput_) {
        glfw_android_set_soft_input_visible(show);
        lastWantTextInput_ = show;
      }
    }
#endif

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
#if defined(__ANDROID__)
      // The backend has already scaled the fonts by the display density (see
      // applyAndroidDensity); this HUD wants more than that, not less of it.
      style.FontScaleDpi = 1.6f * gImGuiDensity;
#else
      style.FontScaleDpi = 1.6f;
#endif
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

#if defined(__ANDROID__)
    // With the widgets done, remember whether this frame's left press landed on
    // a window scrollbar: the next frame's io.WantTextInput is the one it may
    // wrongly raise, so the decision above needs this latch. Any other press
    // (in particular one inside a text field) clears it again.
    {
      ImGuiContext &g = *ImGui::GetCurrentContext();
      if (g.IO.MouseClicked[0]) {
        lastPressWasScrollbar_ = false;
        if (g.ActiveIdIsJustActivated && g.ActiveIdWindow) {
          const ImGuiID sby =
              ImGui::GetWindowScrollbarID(g.ActiveIdWindow, ImGuiAxis_Y);
          const ImGuiID sbx =
              ImGui::GetWindowScrollbarID(g.ActiveIdWindow, ImGuiAxis_X);
          lastPressWasScrollbar_ = (g.ActiveId == sby || g.ActiveId == sbx);
        }
      }
    }
#endif

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  }
  return 1;
}
} // namespace detail
} // namespace v4d
} // namespace cv
