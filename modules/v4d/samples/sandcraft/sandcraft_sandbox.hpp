// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The automaton's state -- Particles.cpp's globals, gathered into one struct.
//
// Everything mutable that the GUI, the input node and the simulation share.
// One _shared() registration (in SandcraftPlan) covers all of it, because the
// worker and the ImGui node both reach it. A sub-plan re-registering the same
// object is a no-op, which is also how two sub-plans end up contending on the
// *same* mutex rather than on two.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_SANDBOX_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_SANDBOX_HPP_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include <opencv2/core.hpp>

#include "sandcraft_materials.hpp"

namespace sandcraft {

struct Sandbox {
  // The virtual screen: one material per cell, row-major, width_ * height_.
  std::vector<ParticleType> cells_;
  int width_ = 0;
  int height_ = 0;
  bool particleSwaps_ = true;

  // --- the style ---
  //
  // Van Gogh is the default: NanoVG has no single way of putting a colour on
  // the screen -- flat fills, gradients, images and additive composites all
  // exist -- and a falling-sand world is exactly the kind of subject that begs
  // to be painted rather than diagrammed. The flag is a runtime one so the
  // original, honest-to-the-automaton rendering stays one checkbox away, and so
  // the two can be compared without rebuilding.
  bool vanGogh_ = true;
  // How thickly the paint is laid on: the renderer scales the number of strokes
  // it combs through every run of material with this. It exists so that a slow
  // machine thins the paint instead of dropping frames -- Van Gogh with a
  // lighter palette knife.
  float impasto_ = 1.0f;

  // Brush
  ParticleType brush_ = SAND;
  int penSize_ = 2;
  // The cursor in cell coordinates, or (-1, -1) when it is not over the world.
  // This is only the hover position: it is what the brush ring follows and what
  // the input node compares against, never a painting trigger on its own.
  cv::Point2d penCell_{-1.0, -1.0};
  // The last cell a stroke actually painted, or (-1, -1) between strokes. Kept
  // apart from penCell_ because the two answer different questions: this one
  // is where the *stroke* is, so that a drag continues from it, and it is reset
  // whenever the button is up. Conflating the two is what makes a brush that
  // paints on hover possible at all -- a position that both follows the cursor
  // and marks the stroke's origin cannot tell "the pointer is here" from "the
  // button is down here".
  cv::Point2d paintCell_{-1.0, -1.0};

  // The four top-of-screen emitters
  std::array<Emitter, 4> emitters_ = {{
      {WATER, "Water", true, 0.30f},
      {SAND, "Sand", true, 0.30f},
      {SALT, "Salt", true, 0.30f},
      {OIL, "Oil", true, 0.30f},
  }};

  // One-shot commands the GUI raises and the simulation consumes, so that the
  // ImGui node never has to touch the grid itself.
  bool clearRequested_ = false;
  bool randomizeRequested_ = false;
  bool paused_ = false;

  // Panel state
  bool panelOpen_ = true;
  bool helpOpen_ = false;

  // Written by the simulation, read by the HUD.
  int particleCount_ = 0;
  double fps_ = 0.0;
  double lastFrameTime_ = 0.0;
  int simFrames_ = 0;

  // Mirrored from the ImGui node. The ImGui context only exists on the display
  // thread, so the worker cannot ask ImGui whether it owns the pointer or the
  // keyboard; the GUI copies the flags (and the modifier keys) here and the
  // input node reads them, exactly as shadertoy-editor.cpp does for
  // WantCaptureKeyboard.
  bool guiWantsMouse_ = false;
  bool guiWantsKeyboard_ = false;
  bool shiftDown_ = false;
  bool ctrlDown_ = false;
  bool altDown_ = false;

  // The RNG. Upstream used utils::rand(), a std::mt19937 behind a singleton; a
  // member keeps the port single-threaded and its state next to its user.
  std::mt19937 rng_{std::random_device{}()};

  size_t index(int x, int y) const {
    return size_t(y) * size_t(width_) + size_t(x);
  }
  bool inside(int x, int y) const {
    return x >= 0 && y >= 0 && x < width_ && y < height_;
  }
  ParticleType at(int x, int y) const {
    return inside(x, y) ? cells_[index(x, y)] : NOTHING;
  }
  void put(int x, int y, ParticleType t) {
    if (inside(x, y))
      cells_[index(x, y)] = t;
  }

  // rand() % n, which is what every probability in the automaton is written in
  // terms of. The modulo bias is irrelevant here, and the original had exactly
  // the same distribution.
  unsigned rand(unsigned n) { return n == 0 ? 0u : rng_() % n; }

  void allocate(int w, int h) {
    width_ = w;
    height_ = h;
    cells_.assign(size_t(w) * size_t(h), NOTHING);
  }

  void clear() { std::fill(cells_.begin(), cells_.end(), NOTHING); }
};

} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_SANDBOX_HPP_
