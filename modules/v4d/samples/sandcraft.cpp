// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// Sandcraft -- a falling-sand cellular automaton, ported to Plan-V4D.
//
// Upstream is https://github.com/kallaballa/Sandcraft: an SDL2 + agar program
// that Emscripten compiled to WebAssembly and that paired two players over a
// websocket signalling server and a WebRTC data channel. Three things changed
// in the port:
//
//   * The networking is gone. Nexus, WebRTC, the line-event queue that fed
//     them and the host/guest split (GameState::isHost_) have no meaning in a
//     single-process demo, so what is left of Particles.cpp is the automaton.
//   * The agar GUI is ImGui. The vertical button strip becomes a material
//     palette, the grey bar along the bottom becomes the panel's sections, and
//     the modal "about" window becomes a closable ImGui window.
//   * The world is drawn with nvg(), not blitted. Upstream set one SDL pixel
//     per particle. Here the world is a grid of cells that is *fitted* to the
//     viewport, so the same program is correct at 640x360, at 4K, and after a
//     window resize, and every material gets vector treatment: merged scanline
//     runs instead of per-cell quads, lit-from-above shading, gradients for the
//     liquids, and additive glow for fire, embers and electricity.
//
// The automaton itself is a direct transcription of Particles.cpp, including
// its naming (MOVED-prefixed types, "virtual screen", the inert and floating
// bounds), so the two can be diffed against each other. Where the transcription
// needed to depart from a literal reading -- the original's `rand() >=
// RAND_MAX / 13` is a twelve-in-thirteen gate and not the one-in-thirteen it
// looks like, and its odd-inert types are exempt from the moved/unmoved
// settle -- the departure is called out at the site with the reason, because
// both are silent when you get them wrong: one makes the automaton an order of
// magnitude too slow to settle, the other deletes half the materials.
//
// The sample is split into modules under sandcraft/, each in its own file with
// the section it came from above it; this file is the pipeline that wires them
// together:
//
//   sandcraft_materials.hpp  the type enum, the palette, the brushes, emitters
//   sandcraft_sandbox.hpp    the state the GUI, the input and the sim share
//   sandcraft_automaton.hpp  painting into the grid, and one automaton step
//   sandcraft_view.hpp       fitting the grid to the viewport, pointer to cell
//   sandcraft_grid.hpp       settling moved flavours, bucketing runs
//   sandcraft_vangogh.hpp    the painter's vocabulary: pigments, strokes, glare
//   sandcraft_render.hpp     how one material is drawn, painted or banded
//   sandcraft_scene.hpp      the world slab, the nozzles, the brush ring, HUD
//   sandcraft_post.hpp       the raw-GL varnish: bloom, swirl, impasto, vignette
//   sandcraft_panel.hpp      the ImGui panel and the about window

#include "samples.hpp"

#include <algorithm>
#include <iostream>

#include <opencv2/v4d/v4d.hpp>

#include "sandcraft/sandcraft_automaton.hpp"
#include "sandcraft/sandcraft_grid.hpp"
#include "sandcraft/sandcraft_materials.hpp"
#include "sandcraft/sandcraft_panel.hpp"
#include "sandcraft/sandcraft_post.hpp"
#include "sandcraft/sandcraft_sandbox.hpp"
#include "sandcraft/sandcraft_scene.hpp"
#include "sandcraft/sandcraft_view.hpp"

using namespace cv;
using namespace cv::v4d;
using namespace cv::v4d::event;

namespace sandcraft {

// ===========================================================================
// The sub-plans
// ===========================================================================

// Stage 1 -- read the pointer and the keyboard into the sandbox.
//
// A plain() node rather than an nvg() one: input has nothing to do with the
// graphics context, and the event edges live on the worker.
class SandInputPlan : public V4DPlan {
public:
  Sandbox &sb_;
  SandInputPlan(Sandbox &sb) : sb_(sb) { _shared(sb_); }

  void infer() override {
    plain(&SandInputPlan::apply, RWS(sb_), size_, windowSize_, press_, drag_,
          scroll_, keys_, move_);
  }

  // The pointer and the view have to agree, so the view is recomputed here
  // from the same size_ the renderer uses.
  static void apply(Sandbox &sb, const cv::Size &fbSz, const cv::Size &winSz,
                    const Mouse::List &press, const Mouse::List &drag,
                    const Mouse::List &scroll, const Keyboard::List &keys,
                    const Mouse::List &move) {
    const View v = fitView(fbSz, sb.width_, sb.height_);

    // Where the pointer is, for the brush ring to follow. The move events
    // carry every position, so there is no hover/motion bookkeeping to do.
    // ImGui owns the pointer while it is over a window, and then there is no
    // cell at all -- otherwise dragging a slider would also drag the ring (and
    // paint, see below).
    cv::Point2d windowPt(-1.0, -1.0);
    for (const auto &e : move)
      windowPt = cv::Point2d(e->position().x, e->position().y);
    for (const auto &e : press)
      windowPt = cv::Point2d(e->position().x, e->position().y);

    // --- painting ---
    //
    // A stroke is the polyline through this frame's stroke positions: the
    // press that starts it and every drag that continues it. Nothing else
    // paints, so moving the pointer over the world -- with the button up, which
    // is what a MOVE event is -- changes nothing at all.
    //
    // The trigger is therefore "a press or a drag arrived", and nothing else.
    // Treating the *absence* of drags as the trigger instead (as an earlier
    // reading of "there is no drag event when the pointer is at rest" did) is
    // the same condition with the sense inverted, and it paints on every frame
    // the pointer rests inside the world: the brush follows the cursor and
    // leaves a trail without a click ever happening.
    //
    // sb.paintCell_ is where the stroke currently is, so consecutive frames
    // join up instead of stamping a disc; it is cleared as soon as the button
    // comes up, so the next press starts a fresh stroke rather than ruling a
    // line across the world from wherever the cursor was parked.
    if (sb.guiWantsMouse_) {
      // ImGui owns the pointer; a drag that started over a panel must not
      // paint, and neither must the stroke it interrupted.
      sb.paintCell_ = {-1.0, -1.0};
    } else {
      std::vector<cv::Point2d> stroke;
      stroke.reserve(press.size() + drag.size());
      const auto at = [](const cv::Point &p) {
        return cv::Point2d(double(p.x), double(p.y));
      };
      for (const auto &e : press)
        stroke.push_back(at(e->position()));
      for (const auto &e : drag)
        stroke.push_back(at(e->position()));

      for (const cv::Point2d &p : stroke) {
        const cv::Point2d c = cellAt(v, sb, p, winSz);
        if (c.x < 0.0)
          continue; // dragged off the world; the stroke resumes on re-entry
        // The first point of a stroke is a disc rather than a segment: there is
        // no previous cell to come from, and drawing a zero-length line would
        // leave the head of the stroke unmarked.
        const bool cont = sb.paintCell_.x >= 0.0 && sb.paintCell_.y >= 0.0;
        drawLine(sb, int(c.x), int(c.y), cont ? int(sb.paintCell_.x) : int(c.x),
                 cont ? int(sb.paintCell_.y) : int(c.y), sb.brush_, sb.penSize_);
        sb.paintCell_ = c;
      }
      if (stroke.empty())
        sb.paintCell_ = {-1.0, -1.0}; // the button is up: the stroke is over
    }
    // The ring follows the pointer whether or not it is painting.
    sb.penCell_ = sb.guiWantsMouse_ ? cv::Point2d(-1.0, -1.0)
                                    : cellAt(v, sb, windowPt, winSz);

    // --- keys: the keymap from the original help window ---
    if (!sb.guiWantsKeyboard_) {
      for (const auto &e : keys) {
        switch (e->key()) {
        case Keyboard::DELETE:
          sb.clearRequested_ = true;
          break;
        case Keyboard::LEFT:
          cycleBrush(sb, -1);
          break;
        case Keyboard::RIGHT:
          cycleBrush(sb, +1);
          break;
        case Keyboard::DOWN:
          sb.penSize_ = std::max(1, sb.penSize_ / 2);
          break;
        case Keyboard::UP:
          sb.penSize_ = std::min(32, sb.penSize_ * 2);
          break;
        case Keyboard::ENTER:
          for (Emitter &em : sb.emitters_)
            em.on = !em.on;
          break;
        case Keyboard::BACKSPACE:
          sb.brush_ = NOTHING;
          break;
        case Keyboard::MINUS:
          for (Emitter &em : sb.emitters_)
            em.density = std::max(0.05f, em.density - 0.05f);
          break;
        // '+' and '=' are the same physical key on a US layout.
        case Keyboard::EQUAL:
        case Keyboard::KP_ADD:
          for (Emitter &em : sb.emitters_)
            em.density = std::min(1.0f, em.density + 0.05f);
          break;
        case Keyboard::R:
          sb.randomizeRequested_ = true;
          break;
        case Keyboard::SPACE:
          sb.paused_ = !sb.paused_;
          break;
        // Slots::toggle_tools(): the original's TAB showed and hid the whole
        // dashboard. Here the dashboard is the panel below, so it is the panel
        // that toggles -- and it is the only way back once it is closed, since
        // the close button is on the panel itself.
        case Keyboard::TAB:
          sb.panelOpen_ = !sb.panelOpen_;
          break;
        default:
          break;
        }
      }
    }

    // --- the wheel, with the modifier meanings Slots.cpp gave it ---
    //
    // Slots.cpp maps WHEELUP to *previous*_particle() and WHEELDOWN to
    // *next*_particle(), and GLFW's scroll offset is positive up, so the
    // brush steps backwards on the way up. Getting that sense the other way
    // round is the kind of thing that survives review precisely because it is
    // symmetric -- the wheel still changes the brush, just the wrong way.
    for (const auto &e : scroll) {
      const float dy = e->data().y > 0 ? 1.0f : -1.0f;
      if (sb.shiftDown_) {
        sb.penSize_ =
            std::clamp(int(sb.penSize_ * (dy > 0 ? 2.0f : 0.5f)), 1, 32);
      } else if (sb.ctrlDown_) {
        for (Emitter &em : sb.emitters_)
          em.density = std::clamp(em.density + 0.05f * dy, 0.05f, 1.0f);
      } else if (sb.altDown_) {
        // Slots::toggle_emit() on ALT+wheel, before the brush fallback.
        for (Emitter &em : sb.emitters_)
          em.on = !em.on;
      } else {
        cycleBrush(sb, dy > 0 ? -1 : +1);
      }
    }
  }

  // Slots::previous/next_particle(). Note that the original searches the
  // button table from the end and keeps the brush where it is when the current
  // one is not in the table at all; here an unknown brush simply starts the
  // cycle at the first entry, which is the same place the original would land
  // after one more step.
  static void cycleBrush(Sandbox &sb, int delta) {
    int cur = 0;
    for (size_t i = 0; i < kBrushes.size(); ++i)
      if (kBrushes[i].type == sb.brush_)
        cur = int(i);
    cur = (cur + delta + int(kBrushes.size())) % int(kBrushes.size());
    sb.brush_ = kBrushes[size_t(cur)].type;
  }

private:
  Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
  Property<cv::Size> windowSize_ = P<cv::Size>(V4D::Keys::WINDOW_SIZE);
  Event<Mouse> press_ = E<Mouse>(Mouse::PRESS, Mouse::LEFT);
  Event<Mouse> drag_ = E<Mouse>(Mouse::DRAG, Mouse::LEFT);
  Event<Mouse> scroll_ = E<Mouse>(Mouse::SCROLL);
  Event<Mouse> move_ = E<Mouse>(Mouse::MOVE);
  Event<Keyboard> keys_ = E<Keyboard>(Keyboard::PRESS);
};

// Stage 2 -- the automaton. One plain() node; the heavy work of the frame.
class SandSimulationPlan : public V4DPlan {
public:
  Sandbox &sb_;
  SandSimulationPlan(Sandbox &sb) : sb_(sb) { _shared(sb_); }

  void infer() override { plain(&SandSimulationPlan::step, RWS(sb_)); }

  static void step(Sandbox &sb) {
    // The one-shot commands from the GUI, consumed here so that the ImGui node
    // never has to touch the grid.
    if (sb.clearRequested_) {
      sb.clear();
      sb.clearRequested_ = false;
    }
    if (sb.randomizeRequested_) {
      doRandomLines(sb, sb.brush_);
      sb.randomizeRequested_ = false;
    }
    if (sb.paused_)
      return;

    for (size_t i = 0; i < sb.emitters_.size(); ++i) {
      const Emitter &e = sb.emitters_[i];
      if (e.on)
        emit(sb, emitterColumn(sb.width_, i), 20, e.type, e.density);
    }

    // Keep the top and bottom rows clear so nothing piles against the frame --
    // Particles::logic() does exactly this before stepping, and
    // updateVirtualScreen() then leaves the bottom one alone.
    for (int x = 0; x < sb.width_; ++x) {
      sb.cells_[sb.index(x, 0)] = NOTHING;
      sb.cells_[sb.index(x, sb.height_ - 1)] = NOTHING;
    }

    updateVirtualScreen(sb);
    ++sb.simFrames_;
  }
};

// Stage 3 -- draw, with nvg().
class SandRenderPlan : public V4DPlan {
public:
  Sandbox &sb_;
  SandRenderPlan(Sandbox &sb) : sb_(sb) { _shared(sb_); }

  void infer() override {
    nvg(&SandRenderPlan::draw, RWS(sb_), size_, F(seconds));
  }

  Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

  // The palette is a compile-time table of 36 scalars; there is nothing to pass
  // it in for, so it is built here rather than carried as an edge.
  static void draw(Sandbox &sb, const cv::Size &sz, double timeSec) {
    using namespace cv::v4d::nvg;
    const Palette pal;
    // The colour the window is cleared to. In Van Gogh mode the studio is a
    // warm umber; otherwise it is the near-black it always was. The painter
    // covers the whole framebuffer either way -- the slab is drawn over this,
    // and around the slab the weave is laid directly onto the clear colour --
    // but what is left at the edges has to belong to the same painting.
    clearScreen(sb.vanGogh_ ? cv::Scalar(24, 17, 9, 255) : cv::Scalar(10, 10, 14, 255));
    const View v = fitView(sz, sb.width_, sb.height_);

    // An exponential average over the frame interval. `seconds()` is the
    // runtime's clock, so this is the presented rate, not the sim rate. The
    // previous frame's timestamp lives in the sandbox rather than in a
    // function-local static: a static here would be one variable shared by
    // every worker rendering this plan, so with more than one worker each
    // would compute the interval against the other's frame.
    if (sb.lastFrameTime_ > 0.0 && timeSec > sb.lastFrameTime_) {
      const double instant = 1.0 / (timeSec - sb.lastFrameTime_);
      sb.fps_ = sb.fps_ > 0.0 ? sb.fps_ * 0.9 + instant * 0.1 : instant;
    }
    sb.lastFrameTime_ = timeSec;

    drawWorld(pal, sb, v, sz, timeSec);
  }
};

} // namespace sandcraft

// ===========================================================================
// The plan
// ===========================================================================
using namespace sandcraft;

class SandcraftPlan : public V4DPlan {
  // One sandbox, shared by the three sub-plans and the ImGui node. A sub-plan
  // re-registering the same object is a no-op -- makeSharedVar() returns early
  // for an address it already knows -- which is also how two sub-plans end up
  // contending on the *same* mutex rather than on two.
  static Sandbox sandbox_;

  cv::Ptr<SandInputPlan> input_;
  cv::Ptr<SandSimulationPlan> sim_;
  cv::Ptr<SandRenderPlan> render_;
  // The oil-paint surface. It is owned per plan instance and touched only by
  // this plan's gl() nodes on this instance's GL context, so -- like CubeScene's
  // scene_ -- it is never shared and needs no mutex.
  VanGoghPost post_;

  Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

public:
  SandcraftPlan() {
    _shared(sandbox_);

    // The runtime builds one plan instance per compute worker *and* one for the
    // display thread, so this constructor runs more than once per process (with
    // run<T>(0) -- a single compute worker -- exactly twice). Building the grid
    // here unconditionally would therefore re-run it per instance, which is
    // harmless only as long as both happen before the first frame; the guard
    // makes "the world is allocated and seeded exactly once" a property of the
    // code rather than a property of the runtime's construction order.
    //
    // r20x180 is the original's 640x360 at half the resolution; nothing in the
    // automaton depends on it, and the renderer fits whatever it is given to
    // the viewport.
    if (sandbox_.cells_.empty()) {
      sandbox_.allocate(640, 360);
      seed();
    }

    input_ = _sub<SandInputPlan>(this, sandbox_);
    sim_ = _sub<SandSimulationPlan>(this, sandbox_);
    render_ = _sub<SandRenderPlan>(this, sandbox_);
  }

  // A starting scene, so the demo opens on something rather than on an empty
  // box: a stone floor, a bank of sand, a pool of water and an iron pillar with
  // a torch on it.
  //
  // The floor spans the full width and sits on the last simulated row, because
  // the row under it is the one the simulation clears every frame -- a floor
  // that stops short of the edges, or that floats above the drain, lets the
  // material the emitters lay down pour off the sides and disappear instead of
  // piling up in front of it.
  static void seed() {
    const int w = sandbox_.width_, h = sandbox_.height_;
    const int floorY = h - 2; // the last row the scan reaches; h-1 is drained
    for (int x = 0; x < w; ++x) {
      sandbox_.put(x, floorY, WALL);
      sandbox_.put(x, floorY - 1, WALL);
    }
    for (int x = w / 8; x < w / 3; ++x)
      for (int y = floorY - 8; y < floorY - 2; ++y)
        sandbox_.put(x, y, SAND);
    for (int x = 5 * w / 8; x < 9 * w / 10; ++x)
      for (int y = floorY - 8; y < floorY - 2; ++y)
        sandbox_.put(x, y, WATER);
    const int pillarX = w / 2 - 10;
    for (int y = floorY - 34; y < floorY - 2; ++y) {
      sandbox_.put(pillarX, y, IRONWALL);
      sandbox_.put(pillarX + 1, y, IRONWALL);
    }
    sandbox_.put(pillarX, floorY - 35, TORCH);
    sandbox_.put(pillarX + 1, floorY - 35, TORCH);
  }

  void gui() override { imgui(drawPanel, RWS(sandbox_)); }

  // Between the two nvg() passes: read the painting back and lay the varnish,
  // the bloom, the living-paint swirl and the impasto sheen over it -- in raw
  // GL, never fb(). Only in Van Gogh mode; the diagram mode keeps its flat cells.
  static void postProcess(Sandbox &sb, VanGoghPost &post) {
    if (sb.vanGogh_)
      post.process();
  }

  // The second nvg() pass: the crisp chrome (brush ring + HUD), laid on top of
  // the finished painting so the cursor and the readouts stay legible.
  static void overlayDraw(Sandbox &sb, const cv::Size &sz) {
    const Palette pal;
    const View v = fitView(sz, sb.width_, sb.height_);
    drawChrome(pal, sb, v, sz);
  }

  void setup() override {
    set(GlobalState::Keys::SHOW_FRAME_TIME, V(false));
    // The offscreen texture + composite shader for the gl() pass, created once
    // per instance in this instance's GL context (the CubeScene pattern).
    gl(&VanGoghPost::init, RW(post_));
    std::cerr << "sandcraft: fb=" << V4D::get<cv::Size>(V4D::Keys::SIZE)
              << " win=" << V4D::get<cv::Size>(V4D::Keys::WINDOW_SIZE)
              << std::endl;
  }

  void teardown() override { gl(&VanGoghPost::destroy, RW(post_)); }

  void infer() override {
    set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(10, 10, 14, 255)));
    // Input, then simulate, then draw: the graph is the pipeline, and the order
    // is the order the sub-plans are spliced in. The whole painting is drawn
    // with nvg() (render_), varnished and set glowing by a gl() pass, and the
    // crisp chrome is laid over it with a second nvg().
    subInfer(input_);
    subInfer(sim_);
    subInfer(render_);                                          // nvg: the painting
    gl(&SandcraftPlan::postProcess, RWS(sandbox_), RW(post_));  // gl: varnish + glow
    nvg(&SandcraftPlan::overlayDraw, RWS(sandbox_), size_);      // nvg: crisp chrome
  }
};

Sandbox SandcraftPlan::sandbox_;

V4D_DEMO_MAIN(int argc, char **argv) {
  cv::v4d::add_asset_search_paths();

  cv::Rect viewport(0, 0, 1280, 720);
  cv::Ptr<V4D> runtime = V4D::init(
      viewport, "Sandcraft", AllocateFlags::NANOVG | AllocateFlags::IMGUI);
  V4DPlan::run<SandcraftPlan>(0);
  return 0;
}
