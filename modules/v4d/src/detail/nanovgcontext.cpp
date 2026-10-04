// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>

#include "opencv2/v4d/detail/nanovgcontext.hpp"
#include "opencv2/v4d/nvg.hpp"
#if defined(OPENCV_V4D_USE_ES3)
#define NANOVG_GLES3_IMPLEMENTATION 1
#else
#define NANOVG_GL3_IMPLEMENTATION 1
#endif

#include "nanovg_gl.h"
#include "nanovg_gl_utils.h"
#include "opencv2/core/utility.hpp"
#include "opencv2/v4d/detail/gl.hpp"

namespace cv {
namespace v4d {
namespace detail {

namespace {

/// Registers one NanoVG font under `name`, looking for it in the asset search
/// path first.
///
/// nvgCreateFont() takes a plain path and fopen()s it, so a font named relative
/// to the source tree only resolves when the process happens to run from the
/// repository root - from anywhere else the call fails, NanoVG silently keeps a
/// font with no glyphs, and every text() in every sample draws nothing without
/// a word of complaint. cv::samples::findFile() is what turns the name into a
/// path that is right from the build tree, from the install tree and from a
/// packaged sample alike, so it goes first.
///
/// Returns the face that was loaded, or an empty string when nothing was found.
std::string registerFont(NVGcontext *ctx, const char *name,
                         const std::vector<std::string> &candidates) {
  for (const auto &candidate : candidates) {
    const cv::String found = cv::samples::findFile(candidate, false, true);
    if (!found.empty() && nvgCreateFont(ctx, name, found.c_str()) >= 0)
      return found;
  }
  return std::string();
}

}  // namespace

NanoVGContext::NanoVGContext(cv::Ptr<FrameBufferContext> fbContext)
    : mainFbContext_(fbContext),
      nvgFbContext_(FrameBufferContext::make("NanoVG", fbContext)),
      context_(nullptr) {
  FrameBufferContext::WindowScope winScope(fbCtx());
  FrameBufferContext::GLScope glScope(fbCtx(), GL_FRAMEBUFFER);
#if defined(OPENCV_V4D_USE_ES3)
  context_ = nvgCreateGLES3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
#else
  context_ = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
#endif
  if (!context_)
    CV_Error(Error::StsError, "Could not initialize NanoVG!");
  // A font that is missing is worth saying out loud once: "no text appears" is
  // the least diagnosable bug there is, and add_asset_search_paths() is what
  // makes these fonts findable in the first place.
  if (registerFont(context_, "icons",
                   {"fonts/entypo.ttf", "entypo.ttf",
                    "modules/v4d/assets/fonts/entypo.ttf"}).empty())
    CV_Error(Error::StsError,
             "Could not load the V4D font 'icons' (entypo.ttf). "
             "Call cv::v4d::add_asset_search_paths() before "
             "V4D::init(), and check that fonts are present.");

  if (registerFont(context_, "sans",
                   {"fonts/Roboto-Regular.ttf", "Roboto-Regular.ttf",
                    "modules/v4d/assets/fonts/Roboto-Regular.ttf",
                    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                    "/usr/share/fonts/TTF/DejaVuSans.ttf",
                    "/usr/share/fonts/truetype/Roboto-Regular.ttf"}).empty())
    CV_Error(Error::StsError,
             "Could not load the V4D font 'sans' (Roboto-Regular.ttf). "
             "Call cv::v4d::add_asset_search_paths() before "
             "V4D::init().");

  if (registerFont(context_, "sans-bold",
                   {"fonts/Roboto-Bold.ttf", "Roboto-Bold.ttf",
                    "modules/v4d/assets/fonts/Roboto-Bold.ttf",
                    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
                    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                    "/usr/share/fonts/truetype/Roboto-Bold.ttf"}).empty())
    CV_Error(Error::StsError,
             "Could not load the V4D font 'sans-bold' (Roboto-Bold.ttf). "
             "Call cv::v4d::add_asset_search_paths() before "
             "V4D::init().");
}

int NanoVGContext::execute(const cv::Rect &vp, std::function<void()> fn) {
  {
    FrameBufferContext::WindowScope winScope(fbCtx());
    FrameBufferContext::GLScope glScope(fbCtx(), GL_FRAMEBUFFER);
    return render(vp, fn);
  }
}

int NanoVGContext::render(const cv::Rect &vp, std::function<void()> fn) {
  {
    glEnable(GL_SCISSOR_TEST);
    glScissor(vp.x, vp.y, vp.width, vp.height);
    glViewport(vp.x, vp.y, vp.width, vp.height);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    NanoVGContext::Scope nvgScope(*this, vp);
    cv::v4d::nvg::detail::NVG::initializeContext(context_);
    //		cv::v4d::nvg::scale(0.5, 0.5);
    fn();
    glDisable(GL_SCISSOR_TEST);

    return 1;
  }
}

void NanoVGContext::begin(const cv::Rect &viewport) {
  CV_UNUSED(viewport);
  float w = fbCtx()->size().width;
  float h = fbCtx()->size().height;
  float r = fbCtx()->pixelRatioX();
  nvgSave(context_);
  nvgBeginFrame(context_, w, h, r);
}

void NanoVGContext::end() {
  // FIXME make nvgCancelFrame possible

  nvgEndFrame(context_);
  nvgRestore(context_);
}

cv::Ptr<FrameBufferContext> NanoVGContext::fbCtx() { return nvgFbContext_; }
} // namespace detail
} // namespace v4d
} // namespace cv
