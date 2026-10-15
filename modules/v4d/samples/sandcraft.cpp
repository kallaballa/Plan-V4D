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

#include "samples.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <opencv2/v4d/v4d.hpp>

using std::string;
using std::vector;
using namespace cv;
using namespace cv::v4d;
using namespace cv::v4d::event;

namespace sandcraft {

// ===========================================================================
// The material set -- Particles.hpp, verbatim
// ===========================================================================
//
// Enumerating conventions, carried over unchanged:
//   inert:    [INERT_LOWER_BOUND,    INERT_UPPER_BOUND]
//   floating: [FLOATING_LOWER_BOUND, FLOATING_UPPER_BOUND]
//
// Odd values are the "moved" flavour of the even type below them: the renderer
// draws `t - 1` for an odd `t` and settles the cell back to `t - 1`, which is
// what makes a material blink between resting and moving.
static constexpr int INERT_LOWER_BOUND = 0;
static constexpr int INERT_UPPER_BOUND = 13;
static constexpr int FLOATING_LOWER_BOUND = 30;
static constexpr int FLOATING_UPPER_BOUND = 33;

enum ParticleType : uint8_t {
  // inert
  NOTHING = 0,
  WALL,
  IRONWALL,
  TORCH,
  STOVE,
  ICE,
  RUST,
  EMBER,
  PLANT,
  NIL,

  // spouts
  WATERSPOUT,
  SANDSPOUT,
  SALTSPOUT,
  OILSPOUT,

  // elemental
  WATER,
  MOVEDWATER,
  DIRT,
  MOVEDDIRT,
  SALT,
  MOVEDSALT,
  OIL,
  MOVEDOIL,
  SAND,
  MOVEDSAND,

  // combined
  SALTWATER,
  MOVEDSALTWATER,
  MUD,
  MOVEDMUD,
  ACID,
  MOVEDACID,

  // floating
  STEAM,
  MOVEDSTEAM,
  FIRE,
  MOVEDFIRE,

  // electricity
  ELEC,
  MOVEDELEC,

  PARTICLE_TYPE_COUNT
};

static inline bool isInert(ParticleType t) {
  return t >= INERT_LOWER_BOUND && t <= INERT_UPPER_BOUND;
}
static inline bool isFloating(ParticleType t) {
  return t >= FLOATING_LOWER_BOUND && t <= FLOATING_UPPER_BOUND;
}
// Burnable materials: plants and oils.
static inline bool isBurnable(ParticleType t) {
  return t == PLANT || t == OIL || t == MOVEDOIL;
}
// ... of which only the plant smoulders rather than going up in flames.
static inline bool burnsAsEmber(ParticleType t) { return t == PLANT; }

// ===========================================================================
// The palette -- Palette.hpp, with SDL_MapRGB replaced by cv::Scalar
// ===========================================================================
//
// NanoVG's fill and stroke take BGRA, which is exactly cv::Scalar's channel
// order, so the RGB triples are carried over unchanged and only the
// interpretation of the four floats differs.
class Palette {
public:
  cv::Scalar operator[](ParticleType t) const { return color_[t]; }

private:
  std::array<cv::Scalar, PARTICLE_TYPE_COUNT> color_ = [] {
    std::array<cv::Scalar, PARTICLE_TYPE_COUNT> c{};
    // The SDL_MapRGB(r, g, b) arguments of the original, re-ordered for BGRA.
    const auto set = [&c](ParticleType t, int r, int g, int b) {
      c[t] = cv::Scalar(b, g, r, 255);
    };
    // inert
    set(SAND, 238, 204, 128);
    set(WALL, 100, 100, 100);
    set(NIL, 60, 60, 60);
    set(IRONWALL, 110, 110, 110);
    set(TORCH, 139, 69, 19);
    set(STOVE, 74, 74, 74);
    set(ICE, 175, 238, 238);
    set(PLANT, 0, 150, 0);
    set(EMBER, 127, 25, 25);
    set(RUST, 110, 40, 10);
    // elemental
    set(WATER, 32, 32, 255);
    set(DIRT, 205, 175, 149);
    set(SALT, 255, 255, 255);
    set(OIL, 128, 64, 64);
    // combined
    set(MUD, 139, 69, 19);
    set(SALTWATER, 65, 105, 225);
    set(STEAM, 95, 158, 160);
    // extra
    set(ACID, 173, 255, 47);
    set(FIRE, 255, 50, 50);
    set(ELEC, 255, 255, 0);
    // spouts
    set(WATERSPOUT, 0, 0, 128);
    set(SANDSPOUT, 240, 230, 140);
    set(SALTSPOUT, 238, 233, 233);
    set(OILSPOUT, 108, 44, 44);
    // The empty cell is the background of the world rather than a material: a
    // cold near-black that the panel chrome can sit on.
    set(NOTHING, 14, 14, 20);
    return c;
  }();
};

// ===========================================================================
// The brush palette -- Buttons.hpp, as data
// ===========================================================================
//
// Upstream this was 19 AG_Buttons that a callback indexed into. Here it is the
// same 19 entries in the same order, consumed by the ImGui palette grid and by
// the LEFT/RIGHT brush-cycling keys.
struct BrushInfo {
  const char *name;
  ParticleType type;
};

static constexpr std::array<BrushInfo, 19> kBrushes = {{
    {"Water", WATER},
    {"Sand", SAND},
    {"Salt", SALT},
    {"Oil", OIL},
    {"Fire", FIRE},
    {"Acid", ACID},
    {"Dirt", DIRT},
    {"Water spout", WATERSPOUT},
    {"Sand spout", SANDSPOUT},
    {"Salt spout", SALTSPOUT},
    {"Oil spout", OILSPOUT},
    {"Wall", WALL},
    {"Torch", TORCH},
    {"Stove", STOVE},
    {"Plant", PLANT},
    {"Ice", ICE},
    {"Iron wall", IRONWALL},
    {"Void", NIL},
    {"Erase", NOTHING},
}};

static const char *brushName(ParticleType t) {
  for (const BrushInfo &b : kBrushes)
    if (b.type == t)
      return b.name;
  return "?";
}

// ===========================================================================
// The emitters -- GameState.hpp's emit*/dens* flags, gathered into one struct
// ===========================================================================
struct Emitter {
  ParticleType type;
  const char *name;
  bool on = true;
  float density = 0.3f;
};

// ===========================================================================
// The automaton -- Particles.cpp
// ===========================================================================
//
// Everything mutable that the GUI, the input node and the simulation share.
// One _shared() registration (in SandcraftPlan) covers all of it, because the
// worker and the ImGui node both reach it.
struct Sandbox {
  // The virtual screen: one material per cell, row-major, width_ * height_.
  vector<ParticleType> cells_;
  int width_ = 0;
  int height_ = 0;
  bool particleSwaps_ = true;

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

// ---------------------------------------------------------------------------
// Drawing into the grid -- Particles.cpp
// ---------------------------------------------------------------------------

// Emitting a material at (x, 1) `width` cells wide, each cell with probability
// `p`.
static void emit(Sandbox &sb, int x, int width, ParticleType type, float p) {
  for (int i = x - width / 2; i < x + width / 2; ++i) {
    if (float(sb.rand(1u << 20)) / float(1u << 20) < p)
      sb.put(i, 1, type);
  }
}

// A filled disc of `radius` cells, clipped to the grid.
static void drawParticles(Sandbox &sb, int xpos, int ypos, int radius,
                          ParticleType type) {
  const int y0 = std::max(0, ypos - radius - 1);
  const int y1 = std::min(sb.height_ - 1, ypos + radius);
  for (int y = y0; y <= y1; ++y) {
    for (int x = std::max(0, xpos - radius - 1);
         x <= std::min(sb.width_ - 1, xpos + radius); ++x) {
      const int dx = x - xpos, dy = y - ypos;
      if (dx * dx + dy * dy <= radius * radius)
        sb.put(x, y, type);
    }
  }
}

// A brush stroke. The pen radius is the pen size, so pen size 2 paints a disc
// five cells across.
static void drawLine(Sandbox &sb, int newx, int newy, int oldx, int oldy,
                     ParticleType type, int penSize) {
  if (newx == oldx && newy == oldy) {
    drawParticles(sb, newx, newy, penSize, type);
    return;
  }
  const int dx = std::abs(newx - oldx), dy = std::abs(newy - oldy);
  const float step = 1.0f / float(std::max(dx, dy));
  for (float a = 0.0f; a < 1.0f; a += step)
    drawParticles(sb, int(a * newx + (1 - a) * oldx),
                  int(a * newy + (1 - a) * oldy), penSize, type);
}

// Particles::doRandomLines: forty random strokes across the whole grid.
static void doRandomLines(Sandbox &sb, ParticleType type) {
  for (int i = 0; i < 20; ++i)
    drawLine(sb, int(sb.rand(unsigned(sb.width_))), 0,
             int(sb.rand(unsigned(sb.width_))), sb.height_ - 1, type,
             sb.penSize_);
  for (int i = 0; i < 20; ++i)
    drawLine(sb, 0, int(sb.rand(unsigned(sb.height_))), sb.width_ - 1,
             int(sb.rand(unsigned(sb.height_))), type, sb.penSize_);
}

// ---------------------------------------------------------------------------
// The rules -- Particles.cpp
// ---------------------------------------------------------------------------

// What an inert cell does. Note that the spouts (10..13) fall inside the inert
// range and are handled here, not in moveParticle(): they are fixed cells that
// shed a moving particle downwards.
static void inertParticleLogic(Sandbox &sb, int x, int y, ParticleType type) {
  int index, above, left, right, below, abovetwo;
  switch (type) {
  case NIL: // the eraser: clear the four-neighbourhood
    above = x + ((y - 1) * sb.width_);
    left = (x + 1) + (y * sb.width_);
    right = (x - 1) + (y * sb.width_);
    below = x + ((y + 1) * sb.width_);
    if (sb.cells_[above] != NOTHING)
      sb.cells_[above] = NOTHING;
    if (sb.cells_[below] != NOTHING)
      sb.cells_[below] = NOTHING;
    if (sb.cells_[left] != NOTHING)
      sb.cells_[left] = NOTHING;
    if (sb.cells_[right] != NOTHING)
      sb.cells_[right] = NOTHING;
    break;
  case IRONWALL: // rusts where it touches rust
    above = x + ((y - 1) * sb.width_);
    left = (x + 1) + (y * sb.width_);
    right = (x - 1) + (y * sb.width_);
    if (sb.rand(200) == 0 && (sb.cells_[above] == RUST ||
                               sb.cells_[left] == RUST ||
                               sb.cells_[right] == RUST))
      sb.cells_[x + (y * sb.width_)] = RUST;
    break;
  case TORCH: // sets fire to its three open neighbours, boils water
    above = x + ((y - 1) * sb.width_);
    left = (x + 1) + (y * sb.width_);
    right = (x - 1) + (y * sb.width_);
    if (sb.rand(2) == 0) {
      if (sb.cells_[above] == NOTHING || sb.cells_[above] == MOVEDFIRE)
        sb.cells_[above] = MOVEDFIRE;
      if (sb.cells_[right] == NOTHING || sb.cells_[right] == MOVEDFIRE)
        sb.cells_[right] = MOVEDFIRE;
      if (sb.cells_[left] == NOTHING || sb.cells_[left] == MOVEDFIRE)
        sb.cells_[left] = MOVEDFIRE;
    }
    if (sb.cells_[above] == MOVEDWATER || sb.cells_[above] == WATER)
      sb.cells_[above] = MOVEDSTEAM;
    if (sb.cells_[right] == MOVEDWATER || sb.cells_[right] == WATER)
      sb.cells_[right] = MOVEDSTEAM;
    if (sb.cells_[left] == MOVEDWATER || sb.cells_[left] == WATER)
      sb.cells_[left] = MOVEDSTEAM;
    break;
  case PLANT: // creeps into adjacent water
    if (sb.rand(2) == 0) {
      switch (sb.rand(4)) {
      case 0: index = (x - 1) + (y * sb.width_); break;
      case 1: index = x + ((y - 1) * sb.width_); break;
      case 2: index = (x + 1) + (y * sb.width_); break;
      default: index = x + ((y + 1) * sb.width_); break;
      }
      if (sb.cells_[index] == WATER)
        sb.cells_[index] = PLANT;
    }
    break;
  case EMBER: // ignites what it can, burns out
    below = x + ((y + 1) * sb.width_);
    if (sb.cells_[below] == NOTHING || isBurnable(sb.cells_[below]))
      sb.cells_[below] = FIRE;
    switch (sb.rand(4)) {
    case 0: index = (x - 1) + (y * sb.width_); break;
    case 1: index = x + ((y - 1) * sb.width_); break;
    case 2: index = (x + 1) + (y * sb.width_); break;
    default: index = x + ((y + 1) * sb.width_); break;
    }
    if (sb.cells_[index] == PLANT)
      sb.cells_[index] = FIRE;
    if (sb.rand(18) == 0)
      sb.cells_[x + (y * sb.width_)] = NOTHING;
    break;
  case STOVE: // boils, separates saltwater, ignites oil
    above = x + ((y - 1) * sb.width_);
    abovetwo = x + ((y - 2) * sb.width_);
    if (sb.rand(4) == 0 && sb.cells_[above] == WATER)
      sb.cells_[above] = STEAM;
    if (sb.rand(4) == 0 && sb.cells_[above] == SALTWATER) {
      sb.cells_[above] = SALT;
      sb.cells_[abovetwo] = STEAM;
    }
    if (sb.rand(8) == 0 && sb.cells_[above] == OIL)
      sb.cells_[above] = EMBER;
    break;
  case RUST: // deteriorates
    if (sb.rand(7000) == 0)
      sb.cells_[x + (y * sb.width_)] = NOTHING;
    break;

  // --- spouts ---
  case WATERSPOUT:
    if (sb.rand(6) == 0) {
      below = x + ((y + 1) * sb.width_);
      if (sb.cells_[below] == NOTHING)
        sb.cells_[below] = MOVEDWATER;
    }
    break;
  case SANDSPOUT:
    if (sb.rand(6) == 0) {
      below = x + ((y + 1) * sb.width_);
      if (sb.cells_[below] == NOTHING)
        sb.cells_[below] = MOVEDSAND;
    }
    break;
  case SALTSPOUT:
    if (sb.rand(6) == 0) {
      below = x + ((y + 1) * sb.width_);
      if (sb.cells_[below] == NOTHING)
        sb.cells_[below] = MOVEDSALT;
      if (sb.cells_[below] == WATER || sb.cells_[below] == MOVEDWATER)
        sb.cells_[below] = MOVEDSALTWATER;
    }
    break;
  case OILSPOUT:
    if (sb.rand(6) == 0) {
      below = x + ((y + 1) * sb.width_);
      if (sb.cells_[below] == NOTHING)
        sb.cells_[below] = MOVEDOIL;
    }
    break;
  default:
    break;
  }
}

// The movement rule of a single cell. `type` arrives as the *resting* material
// and is incremented to its MOVED flavour here, which is why the enum keeps
// every material and its moved twin adjacent.
//
// The block at the end marked "realism" is the density ordering: a liquid sinks
// through anything denser than itself, and swaps with it. Upstream made it a
// Config flag because it was added late; here it is a checkbox in the panel.
static void moveParticle(Sandbox &sb, int x, int y, ParticleType type) {
  type = ParticleType(type + 1);

  const int above = x + ((y - 1) * sb.width_);
  const int same = x + (sb.width_ * y);
  const int below = x + ((y + 1) * sb.width_);
  auto &c = sb.cells_;

  // Gravity, or its reverse for the floating types.
  if (!isFloating(type)) {
    if (c[below] == NOTHING && sb.rand(8)) {
      c[below] = type;
      c[same] = NOTHING;
      return;
    }
  } else {
    if (sb.rand(3) == 0) // slow down, please
      return;
    if ((c[above] == NOTHING || c[above] == FIRE) && sb.rand(8) &&
        c[same] != ELEC && c[same] != MOVEDELEC) {
      if (type == MOVEDFIRE && sb.rand(20) == 0)
        c[same] = NOTHING;
      else {
        c[above] = c[same];
        c[same] = NOTHING;
      }
      return;
    }
  }

  // Pick a side to try first.
  const int sign = sb.rand(2) == 0 ? -1 : 1;
  const int first = (x + sign) + (sb.width_ * y);
  const int second = (x - sign) + (sb.width_ * y);

  int index = 0;
  switch (type) {
  case MOVEDELEC:
    if (sb.rand(2) == 0)
      c[same] = NOTHING;
    break;
  case MOVEDSTEAM:
    if (sb.rand(1000) == 0) { // condenses
      c[same] = MOVEDWATER;
      return;
    }
    if (sb.rand(500) == 0) { // dissipates
      c[same] = NOTHING;
      return;
    }
    if (!isInert(c[above]) && !isFloating(c[above])) {
      // carries whatever it is rising through along with it
      if (sb.rand(15) == 0) {
        c[same] = NOTHING;
        return;
      } else {
        c[same] = c[above];
        c[above] = MOVEDSTEAM;
        return;
      }
    }
    break;
  case MOVEDFIRE:
    if (!isBurnable(c[above]) && sb.rand(10) == 0) {
      c[same] = NOTHING;
      return;
    }
    // let the snowman melt
    if (sb.rand(4) == 0) {
      if (c[above] == ICE) {
        c[above] = WATER;
        c[same] = NOTHING;
      }
      if (c[below] == ICE) {
        c[below] = WATER;
        c[same] = NOTHING;
      }
      if (c[first] == ICE) {
        c[first] = WATER;
        c[same] = NOTHING;
      }
      if (c[second] == ICE) {
        c[second] = WATER;
        c[same] = NOTHING;
      }
    }
    // burn whatever is adjacent
    switch (sb.rand(4)) {
    case 0: index = above; break;
    case 1: index = below; break;
    case 2: index = first; break;
    default: index = second; break;
    }
    if (isBurnable(c[index]))
      c[index] = burnsAsEmber(c[index]) ? EMBER : FIRE;
    break;
  case MOVEDWATER:
    if (sb.rand(200) == 0 && c[below] == IRONWALL)
      c[below] = RUST;
    if (c[below] == FIRE || c[above] == FIRE || c[first] == FIRE ||
        c[second] == FIRE)
      c[same] = MOVEDSTEAM;
    // water + dirt -> mud
    if (c[below] == DIRT) {
      c[below] = MOVEDMUD;
      c[same] = NOTHING;
    }
    if (c[above] == DIRT) {
      c[above] = MOVEDMUD;
      c[same] = NOTHING;
    }
    // water + salt -> saltwater
    if (c[above] == SALT || c[above] == MOVEDSALT) {
      c[above] = MOVEDSALTWATER;
      c[same] = NOTHING;
    }
    if (c[below] == SALT || c[below] == MOVEDSALT) {
      c[below] = MOVEDSALTWATER;
      c[same] = NOTHING;
    }
    if (sb.rand(60) == 0) { // melts ice
      switch (sb.rand(4)) {
      case 0: index = above; break;
      case 1: index = below; break;
      case 2: index = first; break;
      default: index = second; break;
      }
      if (c[index] == ICE)
        c[index] = WATER;
    }
    break;
  case MOVEDACID:
    switch (sb.rand(4)) {
    case 0: index = above; break;
    case 1: index = below; break;
    case 2: index = first; break;
    default: index = second; break;
    }
    if (c[index] != WALL && c[index] != IRONWALL && c[index] != WATER &&
        c[index] != MOVEDWATER && c[index] != ACID && c[index] != MOVEDACID)
      c[index] = NOTHING;
    break;
  case MOVEDSALT:
    if (sb.rand(20) == 0) {
      switch (sb.rand(4)) {
      case 0: index = above; break;
      case 1: index = below; break;
      case 2: index = first; break;
      default: index = second; break;
      }
      if (c[index] == ICE)
        c[index] = WATER;
    }
    break;
  case MOVEDSALTWATER:
    // dissolves ice more slowly than plain salt
    if (sb.rand(40) == 0) {
      switch (sb.rand(4)) {
      case 0: index = above; break;
      case 1: index = below; break;
      case 2: index = first; break;
      default: index = second; break;
      }
      if (c[index] == ICE)
        c[index] = WATER;
    }
    break;
  case MOVEDOIL:
    switch (sb.rand(4)) {
    case 0: index = above; break;
    case 1: index = below; break;
    case 2: index = first; break;
    default: index = second; break;
    }
    if (c[index] == FIRE)
      c[same] = FIRE; // keeps burning as it falls
    break;
  default:
    break;
  }

  // "Realism": a lighter material sinks through a heavier one. New cases go
  // here as MOVED_<A> whenever A is lighter than B.
  if (sb.particleSwaps_) {
    switch (type) {
    case MOVEDWATER:
      if (c[above] == SAND || c[above] == MUD ||
          (c[above] == SALTWATER && sb.rand(3) == 0)) {
        c[same] = c[above];
        c[above] = type;
        return;
      }
      break;
    case MOVEDOIL:
      if (c[above] == WATER && sb.rand(3) == 0) {
        c[same] = c[above];
        c[above] = type;
        return;
      }
      break;
    case MOVEDSALTWATER:
      if (c[above] == DIRT || c[above] == MUD ||
          (c[above] == SAND && sb.rand(3) == 0)) {
        c[same] = c[above];
        c[above] = type;
        return;
      }
      break;
    default:
      break;
    }
  }

  // Nothing below: try down-left, down-right, then sideways.
  if (!isFloating(type)) {
    const int firstdown = (x + sign) + ((y + 1) * sb.width_);
    const int seconddown = (x - sign) + ((y + 1) * sb.width_);
    if (c[firstdown] == NOTHING) {
      c[firstdown] = type;
      c[same] = NOTHING;
    } else if (c[seconddown] == NOTHING) {
      c[seconddown] = type;
      c[same] = NOTHING;
    } else if (c[first] == NOTHING) {
      c[first] = type;
      c[same] = NOTHING;
    } else if (c[second] == NOTHING) {
      c[second] = type;
      c[same] = NOTHING;
    }
  } else if (type == MOVEDSTEAM) { // steam drifts sideways as it rises
    const int firstup = (x + sign) + ((y - 1) * sb.width_);
    const int secondup = (x - sign) + ((y - 1) * sb.width_);
    if (c[firstup] == NOTHING) {
      c[firstup] = type;
      c[same] = NOTHING;
    } else if (c[secondup] == NOTHING) {
      c[secondup] = type;
      c[same] = NOTHING;
    } else if (c[first] == NOTHING) {
      c[first] = type;
      c[same] = NOTHING;
    } else if (c[second] == NOTHING) {
      c[second] = type;
      c[same] = NOTHING;
    }
  }
}

static void updateVirtualPixel(Sandbox &sb, int x, int y) {
  const ParticleType same = sb.cells_[sb.index(x, y)];
  if (same == NOTHING)
    return;
  if (isInert(same)) {
    inertParticleLogic(sb, x, y, same);
  } else if ((same % 2) == 0 && sb.rand(13) != 0) {
    // The original's gate is `utils::rand() >= RAND_MAX / 13` over a full-range
    // 32-bit draw, so a cell takes a movement step on about twelve frames out
    // of thirteen. The fall looks uneven because of the odd one out, not
    // because most cells stand still -- reading the expression as "one in
    // thirteen" (which is its shape, not its meaning) makes the whole automaton
    // an order of magnitude too slow to settle: the streams then fall at
    // roughly the rate they are emitted, so material streams straight through
    // the world and drains off the bottom instead of piling up. The even check
    // is what keeps a cell from moving twice in one step.
    moveParticle(sb, x, y, same);
  }
}

// One automaton step: every cell of the play area, scanline by scanline.
//
// The original scans `y < height - dboard_height`, leaving the bottom
// `dboard_height` rows (its dashboard strip) alone, and Particles::logic()
// clears the last simulated row before stepping so that nothing can reach it.
// Here the dashboard is the ImGui panel rather than part of the grid, so the
// equivalent is two cleared rows -- row 0, where the emitters inject, and the
// last one -- and the scan stops one short of the bottom. Stopping short is
// what keeps every neighbour index moveParticle() computes (notably `below` at
// the last row) inside the grid.
static void updateVirtualScreen(Sandbox &sb) {
  const int bottom = sb.height_ - 1; // the cleared row, not scanned
  for (int y = 0; y < bottom; ++y) {
    // Scanning left-to-right biases the automaton, so the direction is
    // redrawn per scanline.
    if (sb.rand(2) == 0)
      for (int x = sb.width_ - 2; x--;)
        updateVirtualPixel(sb, x, y);
    else
      for (int x = 1; x < sb.width_ - 1; ++x)
        updateVirtualPixel(sb, x, y);
  }
}

// ===========================================================================
// Input -- Slots.cpp, minus the event queue that fed the network
// ===========================================================================
//
// The pointer is the only thing that draws, so the mapping from a window
// coordinate to a cell lives in one place and is shared with the renderer: if
// the two ever disagreed, the brush would drift away from the cursor.
struct View {
  cv::Point2f origin_; // top-left of the world slab, in framebuffer pixels
  float cell_ = 1.0f;  // framebuffer pixels per cell
  int width_ = 0, height_ = 0;
  cv::Size fbSize_;    // the drawable area the slab was fitted to
  // The strips reserved above and below the slab for chrome that is drawn in
  // framebuffer pixels: the emitter names above, the title and stats below.
  // They live in the View rather than in the drawing code so that the fitter
  // and the renderer cannot disagree about where the world sits.
  float header_ = 0.0f, footer_ = 0.0f;
};

// The column an emitter sits in. The simulation and the drawn nozzle share it.
//
// Particles::logic() places the four of them at width/2 -+ (width/6)*1 and
// *2, i.e. at 1/6, 2/6, 4/6 and 5/6 of the grid -- an evenly spaced quartet
// across the middle two thirds. The spacing between the four columns is not
// uniform: the middle pair is one sixth apart and the outer pair two sixths
// apart, because the original subtracted 1 and 2 sixths from the centre rather
// than stepping outwards from it.
static int emitterColumn(int gridWidth, size_t i) {
  static const double kColumn[4] = {1.0 / 6.0, 2.0 / 6.0, 4.0 / 6.0, 5.0 / 6.0};
  return int(gridWidth * kColumn[i % 4]);
}

// Fit the grid into the viewport, keeping cells square, leaving a margin and
// reserving a strip above for the emitter names and one below for the HUD.
//
// The bands are what stop the chrome from falling off the window. Fitting the
// world to the whole viewport and centring it leaves a margin of a couple of
// percent on each side, so anything drawn just outside the slab -- a nozzle
// label above it, a stats line below it -- is clipped by the framebuffer, and
// on a 16:9 window the bottom line in particular lands well past the last
// scanline. Reserving the space up front makes the layout correct at any
// window size instead of only at the one it was tuned on.
static View fitView(const cv::Size &sz, int gridW, int gridH) {
  View v;
  v.width_ = std::max(gridW, 1);
  v.height_ = std::max(gridH, 1);
  v.fbSize_ = sz;

  // Scale the chrome with the window, but not without limit: a very small
  // window must not end up with no room left for the world at all.
  const float ui = std::clamp(
      std::min(float(sz.width), float(sz.height)) / 720.0f, 0.55f, 2.0f);
  v.header_ = 30.0f * ui;
  v.footer_ = 46.0f * ui;

  const float availW = std::max(1.0f, float(sz.width));
  const float availH =
      std::max(1.0f, float(sz.height) - v.header_ - v.footer_);
  constexpr float margin = 0.96f;
  v.cell_ =
      std::min(availW / v.width_, availH / v.height_) * margin;
  v.origin_ = cv::Point2f((availW - v.cell_ * v.width_) * 0.5f,
                          v.header_ + (availH - v.cell_ * v.height_) * 0.5f);
  return v;
}

static cv::Point2d windowToCell(const View &v, const cv::Point2d &windowPt) {
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
static cv::Point2d cellAt(const View &v, const Sandbox &sb,
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

// ===========================================================================
// The rendering -- where fb() would have gone, nvg() does instead
// ===========================================================================

// A horizontal run of one material: the run-length merge that replaces the
// per-particle blit. A row of settled sand is one rectangle, not three hundred.
struct Run {
  int x0_, x1_, y_; // inclusive x0_ and x1_
};
using RunBuckets = std::array<vector<Run>, PARTICLE_TYPE_COUNT>;

// Walk the grid once and bucket the runs by the *resting* material, so the
// drawing loop below touches one material at a time and NanoVG gets long
// stretches of identical state to batch. Settling the moved flavour back to
// its resting type is Scene::draw()'s job in the original: the drawing pass is
// what clears the "moving" flag.
//
// The isInert() test is not an optimisation, it is the whole rule. Scene::draw()
// asks `isInert(same)` *first* and only settles inside the else branch, so the
// odd inert types -- WALL, TORCH, ICE, EMBER, NIL and the two odd spouts -- are
// drawn as themselves and keep their value. A "moved" flavour only exists for
// the non-inert half of the enum, because moveParticle() is the only thing that
// ever increments a type and it is only ever called on a non-inert one.
// Applying `raw % 2 ? raw - 1 : raw` to the whole enum turns every odd inert
// type into NOTHING and deletes it, and that is not a subtle degradation: it
// eats the seeded floor, the torches, the ice, and everything painted with the
// Void or Sand-spout brush, in the very first drawing pass.
static inline ParticleType settledType(ParticleType raw) {
  return (!isInert(raw) && (raw % 2) == 1) ? ParticleType(raw - 1) : raw;
}

static int collectRuns(Sandbox &sb, RunBuckets &runs) {
  for (auto &bucket : runs)
    bucket.clear();
  int occupied = 0;
  for (int y = 0; y < sb.height_; ++y) {
    int x = 0;
    while (x < sb.width_) {
      const ParticleType raw = sb.cells_[sb.index(x, y)];
      if (raw == NOTHING) {
        ++x;
        continue;
      }
      const ParticleType type = settledType(raw);
      int x1 = x;
      sb.cells_[sb.index(x, y)] = type;
      while (x1 + 1 < sb.width_) {
        const ParticleType next = sb.cells_[sb.index(x1 + 1, y)];
        if (next == NOTHING || settledType(next) != type)
          break;
        sb.cells_[sb.index(x1 + 1, y)] = type;
        ++x1;
      }
      runs[type].push_back({x, x1, y});
      ++occupied;
      x = x1 + 1;
    }
  }
  return occupied;
}

// How a material is drawn. This is the part with no counterpart upstream: SDL
// had exactly one way to put a colour on the screen, NanoVG has several.
//
// Only resting types are ever asked about: settledType() has already folded
// each moved flavour back into its resting one by the time a bucket is drawn.
enum class Material { SOLID, LIQUID, VAPOUR, EMBERISH, DISSOLVE };

static Material materialOf(ParticleType t) {
  switch (t) {
  case FIRE:
  case EMBER:
  case ELEC:
  case TORCH: // a torch glows: it is what lights the fire around it
    return Material::EMBERISH;
  case STEAM:
    return Material::VAPOUR;
  case ACID:
    return Material::DISSOLVE;
  case WATER:
  case OIL:
  case SALTWATER:
    return Material::LIQUID;
  default:
    return Material::SOLID;
  }
}

// A brighter, warmer version of a colour -- the lit top face of a grain.
static cv::Scalar litFace(cv::Scalar c, float amount) {
  const auto lift = [amount](double v) {
    return std::min(255.0, v + (255.0 - v) * amount);
  };
  return cv::Scalar(lift(c[0]), lift(c[1]), lift(c[2]), c[3]);
}
static cv::Scalar shadeFace(cv::Scalar c, float amount) {
  const auto dim = [amount](double v) { return v * (1.0 - amount); };
  return cv::Scalar(dim(c[0]), dim(c[1]), dim(c[2]), c[3]);
}

// The palette is BGRA and ImVec4 is RGBA, so the same colour crosses over with
// its first two components swapped. Both live in the panel lambda's reach, but
// this is where NanoVG stops and ImGui starts, so the conversion belongs next
// to the palette.
static ImVec4 toImVec(const cv::Scalar &bgra) {
  return ImVec4(float(bgra[2]) / 255.0f, float(bgra[1]) / 255.0f,
                float(bgra[0]) / 255.0f, float(bgra[3]) / 255.0f);
}

// Black lettering on a light swatch, white on a dark one. The palette runs
// from near-black Void (60,60,60) to white Salt and Ice (255,255,255), so a
// single fixed text colour leaves one end of it unreadable.
static ImVec4 contrastOn(const cv::Scalar &bgra) {
  const double lum = 0.2126 * bgra[2] + 0.7152 * bgra[1] + 0.0722 * bgra[0];
  return lum > 140.0 ? ImVec4(0, 0, 0, 1) : ImVec4(1, 1, 1, 1);
}

static void drawMaterial(const Palette &pal, ParticleType type,
                         const vector<Run> &runs, const View &v,
                         float timeSec) {
  using namespace cv::v4d::nvg;
  if (runs.empty())
    return;
  const cv::Scalar base = pal[type];
  const float cell = v.cell_;

  switch (materialOf(type)) {
  case Material::EMBERISH: {
    // Fire, embers, electricity and torches: a dark body, a pulsing radial
    // bloom over it, and a bright core on top.
    const float pulse = 0.65f + 0.35f * std::sin(timeSec * 6.0f);
    beginPath();
    for (const Run &r : runs)
      rect(v.origin_.x + r.x0_ * cell, v.origin_.y + r.y_ * cell,
           (r.x1_ - r.x0_ + 1) * cell, cell);
    fillColor(cv::Scalar(60, 20, 12, 255));
    fill();

    cv::Scalar glow = litFace(base, 0.35f);
    glow[3] = 90.0f * pulse;
    for (const Run &r : runs) {
      const float cx = v.origin_.x + (r.x0_ + r.x1_ + 1) * 0.5f * cell;
      const float cy = v.origin_.y + (r.y_ + 0.5f) * cell;
      beginPath();
      circle(cx, cy, cell * 1.15f);
      fillPaint(
          radialGradient(cx, cy, 0.0f, cell * 1.15f, glow, cv::Scalar(0, 0, 0, 0)));
      fill();
    }

    const float inset = std::min(cell * 0.15f, 1.5f);
    beginPath();
    for (const Run &r : runs)
      rect(v.origin_.x + r.x0_ * cell + inset, v.origin_.y + r.y_ * cell + inset,
           std::max(cell - inset * 2.0f, 1.0f),
           std::max(cell - inset * 2.0f, 1.0f));
    fillColor(base);
    fill();
    break;
  }
  case Material::DISSOLVE: {
    // Acid dissolves rather than covers, so the world shows through it and
    // only a rim marks where it is.
    beginPath();
    for (const Run &r : runs)
      rect(v.origin_.x + r.x0_ * cell, v.origin_.y + r.y_ * cell,
           (r.x1_ - r.x0_ + 1) * cell, cell);
    fillColor(cv::Scalar(0, 0, 0, 0));
    fill();
    beginPath();
    for (const Run &r : runs)
      rect(v.origin_.x + r.x0_ * cell,
           v.origin_.y + (r.y_ + 1) * cell - 1.5f,
           (r.x1_ - r.x0_ + 1) * cell, 1.5f);
    fillColor(base);
    fill();
    break;
  }
  case Material::LIQUID: {
    // A translucent body under one vertical gradient spanning the material's
    // whole extent, which reads as depth rather than as a flat fill.
    int top = runs.front().y_, bottom = runs.back().y_;
    for (const Run &r : runs) {
      top = std::min(top, r.y_);
      bottom = std::max(bottom, r.y_);
    }
    cv::Scalar shallow = litFace(base, 0.28f);
    cv::Scalar deep = shadeFace(base, 0.45f);
    shallow[3] = 190.0f;
    deep[3] = 235.0f;
    beginPath();
    for (const Run &r : runs)
      rect(v.origin_.x + r.x0_ * cell, v.origin_.y + r.y_ * cell,
           (r.x1_ - r.x0_ + 1) * cell, cell);
    fillPaint(linearGradient(0.0f, v.origin_.y + top * cell, 0.0f,
                             v.origin_.y + (bottom + 1) * cell, shallow, deep));
    fill();
    break;
  }
  case Material::VAPOUR: {
    cv::Scalar faint = litFace(base, 0.40f);
    faint[3] = 46.0f;
    fillColor(faint);
    beginPath();
    for (const Run &r : runs) {
      // A horizontal wobble keyed on the cell, so a steam plume is not a
      // stack of identical rectangles.
      const float wob =
          cell * 0.12f * std::sin(timeSec * 2.0f + float(r.x0_ + r.y_) * 0.7f);
      rect(v.origin_.x + r.x0_ * cell - wob, v.origin_.y + r.y_ * cell,
           (r.x1_ - r.x0_ + 1) * cell + wob * 2.0f, cell);
    }
    fill();
    break;
  }
  case Material::SOLID:
  default: {
    // An opaque body with a lit top face and a shaded bottom edge: a one-cell
    // rim light that makes a heap of grains look like a heap and not a block.
    //
    // One path per *band*, not per run. A settled heap is thousands of
    // one-cell runs, so filling each of them separately means thousands of
    // path builds and draw calls for what is three flat rectangles' worth of
    // geometry; NanoVG is far happier with one long path it can scan in one
    // go. The bands never overlap -- they are the top rim, the body and the
    // bottom rim of the same cell -- so painting them in that order is
    // equivalent to painting cell by cell.
    const cv::Scalar top = litFace(base, 0.22f);
    const cv::Scalar bottom = shadeFace(base, 0.28f);
    const float inset = std::min(cell * 0.10f, 1.5f);
    const float body = std::max(cell - inset * 2.0f, 1.0f);
    const float rim = std::max(body * 0.22f, 0.75f);
    // One path per band, each holding every run of this material.
    const auto band = [&](const cv::Scalar &color, float dy, float h) {
      beginPath();
      for (const Run &r : runs)
        rect(v.origin_.x + r.x0_ * cell + inset, v.origin_.y + r.y_ * cell + dy,
             (r.x1_ - r.x0_ + 1) * cell - inset * 2.0f, h);
      fillColor(color);
      fill();
    };
    band(base, inset, body);              // the body
    band(top, inset, rim);                // the lit top face
    band(bottom, inset + body - rim, rim); // the shaded bottom edge
    break;
  }
  }
}

// The world slab, the emitters and the in-world annotations. Everything is in
// framebuffer pixels derived from the fitted cell size, so it is correct at any
// window size.
static void drawWorld(const Palette &pal, Sandbox &sb, const View &v,
                      const cv::Size &sz, double timeSec) {
  using namespace cv::v4d::nvg;
  const float cell = v.cell_;
  const float w = cell * v.width_;
  const float h = cell * v.height_;
  const cv::Point2f o = v.origin_;

  // The slab the world sits in: a rounded frame over a soft shadow, so the
  // automaton reads as an object on the window rather than as the window.
  {
    const Paint shadow = boxGradient(o.x, o.y + h * 0.02f, w, h, 12.0f, 18.0f,
                                     cv::Scalar(0, 0, 0, 170),
                                     cv::Scalar(0, 0, 0, 0));
    beginPath();
    roundedRect(o.x - 6.0f, o.y - 2.0f, w + 12.0f, h + 18.0f, 10.0f);
    fillPaint(shadow);
    fill();

    beginPath();
    roundedRect(o.x - 2.0f, o.y - 2.0f, w + 4.0f, h + 4.0f, 6.0f);
    fillColor(cv::Scalar(30, 30, 38, 255));
    fill();

    beginPath();
    roundedRect(o.x - 2.0f, o.y - 2.0f, w + 4.0f, h + 4.0f, 6.0f);
    strokeColor(cv::Scalar(90, 90, 110, 255));
    strokeWidth(1.5f);
    stroke();
  }

  // The empty field, drawn once so the run pass only has to cover what is
  // actually occupied.
  beginPath();
  rect(o.x, o.y, w, h);
  fillColor(cv::Scalar(16, 16, 22, 255));
  fill();

  // --- the emitters, as nozzles above the play area ---
  //
  // The nozzle sits in the header band above the slab and its name above that.
  // Both offsets are relative to the nozzle mouth, not to the slab edge, so
  // that a world fitted at any size still leaves the label on screen: the
  // band is as tall as the label needs plus the nozzle plus the world margin.
  fontFace("sans");
  const float nozzleFont = std::max(11.0f, std::min(cell * 1.4f, 20.0f));
  fontSize(nozzleFont);
  textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
  for (size_t i = 0; i < sb.emitters_.size(); ++i) {
    const Emitter &e = sb.emitters_[i];
    const float cx = o.x + (emitterColumn(sb.width_, i) + 0.5f) * cell;
    const float cy = o.y - 8.0f;
    const cv::Scalar col = pal[e.type];

    beginPath();
    moveTo(cx - cell * 0.8f, cy - cell * 1.1f);
    lineTo(cx + cell * 0.8f, cy - cell * 1.1f);
    lineTo(cx + cell * 0.45f, cy);
    lineTo(cx - cell * 0.45f, cy);
    closePath();
    fillColor(e.on ? col : cv::Scalar(50, 50, 58, 255));
    fill();

    if (e.on) {
      const float glow = 0.5f + 0.5f * std::sin(timeSec * 4.0f + float(i));
      cv::Scalar g = col;
      g[3] = 70.0f + 60.0f * glow;
      const float gy = cy - cell * 0.55f;
      beginPath();
      circle(cx, gy, cell * 1.0f);
      fillPaint(radialGradient(cx, gy, 0.0f, cell * 1.0f, g, cv::Scalar(0, 0, 0, 0)));
      fill();
    }

    // The name goes clear of the nozzle rather than on top of it: the nozzle
    // reaches cell*1.1 above the mouth, so the label's baseline has to sit
    // above that or the two overlap at small cell sizes.
    cv::Scalar label = col;
    label[3] = e.on ? 230.0f : 90.0f;
    fillColor(label);
    text(cx, cy - cell * 1.1f - 3.0f, e.name, e.name + std::strlen(e.name));
  }

  // --- the grid ---
  RunBuckets runs;
  sb.particleCount_ = collectRuns(sb, runs);

  // A fixed paint order beats sorting a few hundred runs every frame: the
  // structure first, the liquids over it, the glowing and dissolving ones on
  // top. `runs` is indexed by the *resting* type (settledType() above), so this
  // lists every resting material there is, and the array is sized to the list
  // so that a missing entry cannot silently leave the tail at NOTHING.
  //
  // It has to be exhaustive: a material that is missing here is simply never
  // drawn, so the brush that paints it appears broken. Spouts and Void were
  // exactly that -- five of the nineteen brushes laid down cells that the
  // world then drew as empty background.
  static const std::array<ParticleType, 24> kPaintOrder = {
      // structure and inert fixtures
      WALL,   IRONWALL, STOVE,     RUST,     NIL,        WATERSPOUT,
      SANDSPOUT, SALTSPOUT, OILSPOUT, TORCH,
      // bulk solids
      ICE,    PLANT,    DIRT,      MUD,      SALT,       SAND,
      // liquids, over the solids they pool on
      OIL,    WATER,    SALTWATER,
      // the glowing and dissolving ones last, so they sit on top of everything
      STEAM,  FIRE,     EMBER,     ELEC,     ACID};
  for (ParticleType t : kPaintOrder)
    drawMaterial(pal, t, runs[t], v, float(timeSec));

  // --- the brush ring ---
  if (sb.penCell_.x >= 0.0 && cell > 0.75f) {
    const float r = (sb.penSize_ + 0.5f) * cell;
    const float cx = o.x + (sb.penCell_.x + 0.5f) * cell;
    const float cy = o.y + (sb.penCell_.y + 0.5f) * cell;
    const cv::Scalar col = pal[sb.brush_];
    cv::Scalar halo = col;
    halo[3] = 60.0f;
    beginPath();
    circle(cx, cy, r);
    fillPaint(radialGradient(cx, cy, r * 0.6f, r, halo, cv::Scalar(0, 0, 0, 0)));
    fill();
    beginPath();
    circle(cx, cy, r);
    strokeColor(col);
    strokeWidth(std::max(1.0f, cell * 0.10f));
    stroke();
  }

  // --- the HUD, in the framebuffer's own coordinates and in the footer band ---
  //
  // Anchored to the foot of the reserved band rather than to a fixed offset
  // below the slab: `o.y + h` is the bottom of the world, and a stats line
  // placed a constant distance under it lands outside the framebuffer on any
  // window short enough that the fit leaves less room than that distance.
  const float hudTop = o.y + h + 6.0f;
  fontFace("sans-bold");
  fontSize(16.0f);
  textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
  fillColor(cv::Scalar(225, 225, 240, 235));
  const string title = "Sandcraft";
  text(o.x, hudTop, title.c_str(), title.c_str() + title.size());

  fontFace("sans");
  fontSize(12.5f);
  fillColor(cv::Scalar(150, 150, 170, 230));
  char stats[256];
  std::snprintf(stats, sizeof(stats),
                "%d particles   %.0f fps   %dx%d cells   %s, size %d",
                sb.particleCount_, sb.fps_, sb.width_, sb.height_,
                brushName(sb.brush_), sb.penSize_);
  text(o.x, hudTop + 20.0f, stats, stats + std::strlen(stats));

  textAlign(NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
  constexpr const char *kHint =
      "drag to paint   wheel: brush   SHIFT+wheel: pen   DELETE: clear";
  fillColor(cv::Scalar(120, 120, 140, 200));
  text(sz.width - o.x, hudTop + 20.0f, kHint, kHint + std::strlen(kHint));
}

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
      vector<cv::Point2d> stroke;
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
    clearScreen(cv::Scalar(10, 10, 14, 255));
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
    // 320x180 is the original's 640x360 at half the resolution; nothing in the
    // automaton depends on it, and the renderer fits whatever it is given to
    // the viewport.
    if (sandbox_.cells_.empty()) {
      sandbox_.allocate(320, 180);
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

  void gui() override {
    imgui(
        [](Sandbox &sb) {
          using namespace ImGui;
          const Palette pal;

          // ---- the material palette: agar's button strip, as swatches ----
          //
          // One window. Two ImGui windows opened with a plain Begin() and no
          // position both land on the default spot and stack on top of each
          // other, and because they are drawn in call order the second one
          // hides the first: the panel's sections and the readouts underneath
          // it are both unusable, and neither can be moved out from under the
          // other. So the readouts are a section of the panel rather than a
          // second window.
          //
          // First-use positioning rather than Always: the panel floats over the
          // world, and a panel that snaps back to the corner every frame can
          // never be dragged out of the way of whatever it is covering.
          SetNextWindowPos(ImVec2(14, 14), ImGuiCond_FirstUseEver);
          if (Begin("Sandcraft", &sb.panelOpen_)) {
            Text("Material");
            Separator();
            // The swatch is a Button carrying the material's colour, not a
            // Selectable: Selectable paints its background from the style
            // colours only in its hovered and selected states, so a
            // PushStyleColor around it leaves an unselected, unhovered entry
            // looking exactly like the window background -- a grid of labels
            // with no swatches and no indication of what is selected. A Button
            // fills itself with ImGuiCol_Button unconditionally, so the colour
            // always shows, and a marker below it marks the current brush.
            //
            // Seven columns, because the table sizes to its widest label and
            // the names run to "Water spout": at four or five columns the
            // panel is wide enough to sit on top of the leftmost emitter.
            BeginTable("brushes", 7, ImGuiTableFlags_SizingFixedFit |
                                        ImGuiTableFlags_NoSavedSettings);
            for (size_t bi = 0; bi < kBrushes.size(); ++bi) {
              const BrushInfo &b = kBrushes[bi];
              const cv::Scalar c = pal[b.type];
              const bool selected = sb.brush_ == b.type;
              TableNextColumn();
              PushStyleColor(ImGuiCol_Button, toImVec(c));
              PushStyleColor(ImGuiCol_ButtonHovered,
                             toImVec(litFace(c, 0.30f)));
              PushStyleColor(ImGuiCol_ButtonActive, toImVec(litFace(c, 0.55f)));
              char id[32];
              std::snprintf(id, sizeof(id), "##brush%zu", bi);
              if (Button(id, ImVec2(46.0f, 22.0f)))
                sb.brush_ = b.type;
              PopStyleColor(3);
              // The selection marker is drawn by hand rather than through
              // FrameBorderSize/FrameBorderColor: the marker has to survive
              // both the theme (which paints a frame border in a colour that
              // disappears against a light swatch) and the fact that the
              // vendored ImGui has no ImGuiCol_FrameBorder to push at all.
              if (selected) {
                const ImVec2 lo = GetItemRectMin(), hi = GetItemRectMax();
                GetWindowDrawList()->AddRectFilled(
                    ImVec2(lo.x, hi.y + 1.0f), ImVec2(hi.x, hi.y + 4.0f),
                    IM_COL32(255, 255, 255, 230));
              }
              // The name under the swatch, rather than inside it: the swatches
              // are 22px tall and the names run to "Water spout".
              PushStyleColor(ImGuiCol_Text, contrastOn(c));
              TextUnformatted(b.name);
              PopStyleColor(1);
            }
            EndTable();

            Separator();
            Text("Brush");
            SliderInt("Pen size##pen", &sb.penSize_, 1, 32);
            Checkbox("Density swaps##swaps", &sb.particleSwaps_);
            TextDisabled(
                "  let liquids sink through materials denser than "
                "themselves");
            if (Button("Random strokes"))
              sb.randomizeRequested_ = true;
            SameLine();
            if (Button("Clear##clear"))
              sb.clearRequested_ = true;

            Separator();
            Text("Emitters");
            for (size_t i = 0; i < sb.emitters_.size(); ++i) {
              Emitter &e = sb.emitters_[i];
              const cv::Scalar c = pal[e.type];
              char toggle[64], slider[64];
              std::snprintf(toggle, sizeof(toggle), "%s##emit%zu", e.name, i);
              std::snprintf(slider, sizeof(slider), "##density%zu", i);
              PushStyleColor(ImGuiCol_Button, toImVec(c));
              PushStyleColor(ImGuiCol_ButtonHovered,
                             toImVec(litFace(c, 0.30f)));
              PushStyleColor(ImGuiCol_ButtonActive, toImVec(litFace(c, 0.55f)));
              PushStyleColor(ImGuiCol_Text, contrastOn(c));
              const bool pressed = Button(toggle);
              PopStyleColor(4);
              if (pressed)
                e.on = !e.on;
              SameLine();
              TextColored(e.on ? ImVec4(1, 1, 1, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1),
                         "%s", e.on ? "on" : "off");
              PushItemWidth(130.0f);
              SliderFloat(slider, &e.density, 0.05f, 1.0f, "%.2f");
              PopItemWidth();
            }

            Separator();
            Text("World");
            Text("%.0f fps   %d particles   frame %d", sb.fps_,
                 sb.particleCount_, sb.simFrames_);
            Text("%dx%d cells   %s, size %d", sb.width_, sb.height_,
                 brushName(sb.brush_), sb.penSize_);
            Checkbox("Paused##paused", &sb.paused_);
            SameLine();
            if (Button("About##about"))
              sb.helpOpen_ = true;
          }
          End();

          // ---- the about window: the original's modal help dialog ----
          //
          // Begin() is called unconditionally inside the `if`, never as part of
          // it. Written the other way round -- `if (open && Begin(..., &open))`
          // with a separate `if (open) End();` -- the close button clears
          // `open` from inside the window's own contents, so by the time the
          // matching End() is reached the flag is false and the window is left
          // unclosed: ImGui keeps a begun window on its stack forever, and
          // every later window is pushed one level too deep.
          if (sb.helpOpen_) {
            SetNextWindowPos(ImVec2(340, 110), ImGuiCond_Always);
            SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_Always);
            Begin("About Sandcraft", &sb.helpOpen_);
            TextWrapped(
                "A falling-sand sandbox. Materials are laid down with the "
                "brush and then obey the rules of a cellular automaton: sand "
                "piles, water finds a level, fire climbs, salt melts ice, rust "
                "eats iron and plants drink water.");
            Separator();
            Text("Keymap");
            Separator();
            TextDisabled("drag                    paint with the selected brush");
            TextDisabled("wheel, LEFT/RIGHT       previous / next brush");
            TextDisabled("SHIFT + wheel, UP/DOWN  double / halve the pen");
            TextDisabled("CTRL + wheel, -/=       emitter density");
            TextDisabled("ALT + wheel             all emitters on / off");
            TextDisabled("ENTER                  all emitters on / off");
            TextDisabled("BACKSPACE               the eraser");
            TextDisabled("SPACE                   pause the simulation");
            TextDisabled("TAB                    show / hide this panel");
            TextDisabled("R                       a few random strokes");
            TextDisabled("DELETE                  clear the world");
            Separator();
            TextWrapped("Ported from the SDL2 + agar original to Plan-V4D: "
                        "NanoVG for the world, ImGui for the panel, and no "
                        "networking.");
            End();
          }

          // Mirror ImGui's capture flags and modifiers for the worker: the
          // ImGui context only exists on this thread, so the input node cannot
          // ask ImGui itself.
          const ImGuiIO &io = ImGui::GetIO();
          sb.guiWantsMouse_ = io.WantCaptureMouse;
          sb.guiWantsKeyboard_ = io.WantCaptureKeyboard;
          sb.shiftDown_ = io.KeyShift;
          sb.ctrlDown_ = io.KeyCtrl;
          sb.altDown_ = io.KeyAlt;
        },
        RWS(sandbox_));
  }

  void setup() override {
    set(GlobalState::Keys::SHOW_FRAME_TIME, V(false));
    std::cerr << "sandcraft: fb=" << V4D::get<cv::Size>(V4D::Keys::SIZE)
              << " win=" << V4D::get<cv::Size>(V4D::Keys::WINDOW_SIZE)
              << std::endl;
  }

  void infer() override {
    set(V4D::Keys::CLEAR_COLOR, V(cv::Scalar(10, 10, 14, 255)));
    // Input, then simulate, then draw: the graph is the pipeline, and the
    // order is the order the sub-plans are spliced in.
    subInfer(input_);
    subInfer(sim_);
    subInfer(render_);
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
