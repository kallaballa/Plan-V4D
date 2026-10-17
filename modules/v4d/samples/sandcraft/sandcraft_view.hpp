// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// Input -- Slots.cpp, minus the event queue that fed the network, and the
// geometry that goes with it.
//
// The pointer is the only thing that draws, so the mapping from a window
// coordinate to a cell lives in one place and is shared with the renderer: if
// the two ever disagreed, the brush would drift away from the cursor. The
// View is that shared place.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_VIEW_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_VIEW_HPP_

#include <algorithm>
#include <cmath>

#include <opencv2/core.hpp>

#include "sandcraft_sandbox.hpp"

namespace sandcraft {

struct View {
  cv::Point2f origin_; // top-left of the world slab, in framebuffer pixels
  float cell_ = 1.0f;  // framebuffer pixels per cell
  int width_ = 0, height_ = 0;
  cv::Size fbSize_;    // the drawable area the slab was fitted to
};

// The column an emitter sits in. The simulation and the drawn nozzle share it.
//
// Particles::logic() places the four of them at width/2 -+ (width/6)*1 and
// *2, i.e. at 1/6, 2/6, 4/6 and 5/6 of the grid -- an evenly spaced quartet
// across the middle two thirds. The spacing between the four columns is not
// uniform: the middle pair is one sixth apart and the outer pair two sixths
// apart, because the original subtracted 1 and 2 sixths from the centre rather
// than stepping outwards from it.
inline int emitterColumn(int gridWidth, size_t i) {
  // Integer arithmetic, exactly as Particles::logic(): width/2 -+ (width/6)*1
  // and *2. Computing it in floating point moves the first two columns by one
  // cell (e.g. 53/106 instead of 54/107 at width 320).
  const int sixth = gridWidth / 6;
  const int half = gridWidth / 2;
  switch (i % 4) {
  case 0: return half - sixth * 2;
  case 1: return half - sixth;
  case 2: return half + sixth;
  default: return half + sixth * 2;
  }
}

// Fit the grid to the whole viewport, keeping cells square.
//
// The world fills the window: no margin, and no strips reserved above or below
// it. The chrome that used to live in those strips -- the emitter nozzles and
// the HUD -- is painted as a translucent overlay *inside* the world instead
// (drawEmittersOverlay / drawChrome), so nothing is cut out of the play area
// for it. The grid is only ever fitted to the render target, which is the same
// 16:9 as the grid, so the fit is exact and the slab meets every edge; any
// letterbox for a window of another shape is the runtime's aspect-preserving
// blit, not this fit.
inline View fitView(const cv::Size &sz, int gridW, int gridH) {
  View v;
  v.width_ = std::max(gridW, 1);
  v.height_ = std::max(gridH, 1);
  v.fbSize_ = sz;

  const float availW = std::max(1.0f, float(sz.width));
  const float availH = std::max(1.0f, float(sz.height));
  v.cell_ = std::min(availW / v.width_, availH / v.height_);
  v.origin_ = cv::Point2f((availW - v.cell_ * v.width_) * 0.5f,
                          (availH - v.cell_ * v.height_) * 0.5f);
  return v;
}

// The scale the overlay chrome (emitter names, HUD) is drawn at, relative to
// the 720-pixel reference the demo was tuned on. Clamped so a tiny window still
// gets legible text and a huge one does not get absurd chrome.
inline float chromeScale(const cv::Size &sz) {
  return std::clamp(std::min(float(sz.width), float(sz.height)) / 720.0f,
                    0.55f, 2.0f);
}

inline cv::Point2d windowToCell(const View &v, const cv::Point2d &windowPt) {
  return {(windowPt.x - v.origin_.x) / v.cell_,
          (windowPt.y - v.origin_.y) / v.cell_};
}

// The cell under a pointer position, or (-1, -1) when it is outside the world.
//
// GLFW reports the pointer in *window* coordinates while the world is drawn in
// *framebuffer* pixels, and on a scaled desktop those are two different spaces:
// WINDOW_SIZE is what GLFW says the window is, FRAMEBUFFER_SIZE (which is also
// V4D::Keys::SIZE, the drawable area the world is fitted to) is how many pixels
// it actually has. The ratio is therefore applied here, once, rather than
// assumed to be one everywhere else.
inline cv::Point2d cellAt(const View &v, const Sandbox &sb,
                          const cv::Point2d &windowPt, const cv::Size &winSz) {
  if (!sb.inside(1, 1))
    return {-1.0, -1.0};
  const double sx = winSz.width > 0 ? double(v.fbSize_.width) / winSz.width : 1.0;
  const double sy =
      winSz.height > 0 ? double(v.fbSize_.height) / winSz.height : 1.0;
  const cv::Point2d c =
      windowToCell(v, {windowPt.x * sx, windowPt.y * sy});
  const double x = std::floor(c.x), y = std::floor(c.y);
  if (x < 0 || y < 0 || x >= sb.width_ || y >= sb.height_)
    return {-1.0, -1.0};
  return {x, y};
}

} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_VIEW_HPP_
