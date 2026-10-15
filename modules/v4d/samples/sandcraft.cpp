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
    set(SAND, 233, 194, 104);   // wheatfield: chrome yellow
    set(WALL, 104, 118, 134);   // stable stone, a cool blue-grey
    set(NIL, 46, 54, 86);       // void: deep ultramarine ink
    set(IRONWALL, 92, 104, 124); // slate blue
    set(TORCH, 214, 96, 40);    // vermilion flame
    set(STOVE, 86, 70, 52);     // burnt umber iron
    set(ICE, 178, 228, 214);    // pale veridian frost
    set(PLANT, 46, 140, 84);    // viridian cypress
    set(EMBER, 170, 58, 30);    // smouldering orange-red
    set(RUST, 150, 80, 32);     // burnt sienna
    // elemental
    set(WATER, 40, 96, 205);    // cobalt / ultramarine
    set(DIRT, 150, 116, 74);    // raw sienna ochre
    set(SALT, 238, 232, 214);   // cream white
    set(OIL, 130, 112, 54);     // olive-gold
    // combined
    set(MUD, 126, 90, 56);      // wet umber
    set(SALTWATER, 54, 132, 192); // teal
    set(STEAM, 168, 206, 200);  // pale aqua vapour
    // extra
    set(ACID, 188, 212, 64);    // chartreuse
    set(FIRE, 255, 178, 48);    // sun-yellow, hot enough to bloom
    set(ELEC, 255, 238, 96);    // electric chrome yellow
    // spouts
    set(WATERSPOUT, 46, 62, 152);
    set(SANDSPOUT, 240, 220, 148);
    set(SALTSPOUT, 240, 234, 208);
    set(OILSPOUT, 110, 94, 46);
    // The empty cell is the "void" of the night sky the world is painted on: a
    // deep cobalt that the Van Gogh backdrop shows through.
    set(NOTHING, 22, 32, 64);
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

// ===========================================================================
// The Van Gogh treatment -- where the palette becomes paint
// ===========================================================================
//
// NanoVG has no single way of putting a colour on the screen: there are flat
// fills, gradients, tileable images, additive composites and arbitrary paths,
// and all of them are one call away. A flat fill per cell is what the original
// did (one SDL pixel per particle). What follows is what an oil painter would
// have done with the same primitives.
//
// Four ideas, all of them taken from the paintings rather than from a renderer:
//
//   * **A ground, then paint on top of it.** Every oil painting starts with a
//     flat "imprimatura" that covers the canvas and only then receives strokes.
//     Here the ground is one wavy rectangle per run of one material, so the
//     pile is opaque and cheap to draw, and the strokes above it are pure
//     texture. Without it, translucent strokes would let the night sky shine
//     straight through a heap of sand.
//   * **A stroke is a tapered, wobbly ribbon, not a capsule.** A bezier bulged
//     belly with a ragged edge reads as a loaded brush; a capsule reads as a
//     line. The geometry is two beziers out and two beziers back, with a
//     constant half-width and a perpendicular wobble on each edge.
//   * **Shadow, body, light, and one note of the complement.** Van Gogh almost
//     never laid a colour next to itself: blue against orange, yellow against
//     violet, with a thin stroke of the complementary hue as the last passage.
//     Four passes per material is the whole trick.
//   * **The brush flow is combed, and it is stable.** The strokes of a field
//     run in two alternating directions, which is why "Wheatfield with Crows"
//     reads as wind. The direction here is a hash of the cell deformed slowly
//     by time, so it never re-rolls from frame to frame -- a field of dashes
//     whose direction flickers reads as television static.
//
// Everything deterministic comes out of a private hash, never out of the
// Sandbox's RNG: that stream belongs to the automaton, and a renderer that
// consumed it would change the physics by looking at it.
namespace vg {

// ---- a stable hash ---------------------------------------------------------
inline uint32_t hashBits(int a, int b) {
  uint32_t h = (uint32_t(a) * 73856093u) ^ (uint32_t(b) * 19349663u);
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return h;
}
inline float hash01(int a, int b) {
  return float(hashBits(a, b) & 0xFFFFu) / 65535.0f;
}

// ---- colour arithmetic, on cv::Scalar's BGRA channel order -----------------
inline cv::Scalar mix(const cv::Scalar &a, const cv::Scalar &b, float t) {
  return cv::Scalar(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t,
                    a[2] + (b[2] - a[2]) * t, a[3]);
}
inline cv::Scalar withAlpha(const cv::Scalar &c, float a) {
  return cv::Scalar(c[0], c[1], c[2], a);
}
// Lead white, gesso, the cream of a star: the colour reached for whenever
// light itself is the subject.
inline cv::Scalar cream() { return cv::Scalar(226, 240, 250, 255); }
inline cv::Scalar cerulean() { return cv::Scalar(194, 144, 74, 255); }

// ---- the pigments ----------------------------------------------------------
// The palette Van Gogh actually worked from: chrome yellow, cadmium yellow and
// orange, vermilion, emerald green, viridian, Prussian blue, cobalt blue,
// ultramarine, cerulean, lead white, ivory black, burnt sienna and umber.
// Every entry carries light, base and dark -- the three passages a stroke is
// painted with -- and a complement, the thin accent that keeps a field of one
// colour from reading as flat.
struct Pigment {
  cv::Scalar light, base, dark, comp;
};

static const std::array<Pigment, PARTICLE_TYPE_COUNT> &pigmentTable() {
  static const std::array<Pigment, PARTICLE_TYPE_COUNT> table = [] {
    std::array<Pigment, PARTICLE_TYPE_COUNT> p{};
    const auto rgb = [](int r, int g, int b) { return cv::Scalar(b, g, r, 255); };
    const auto set = [&p, &rgb](ParticleType t, int lr, int lg, int lb, int br,
                                int bg, int bb, int dr, int dg, int db) {
      const cv::Scalar base = rgb(br, bg, bb);
      // The complement is the channel swap: blue turns to orange, chrome
      // yellow to violet, vermilion to green. It is a crude complement, and at
      // the weight it is used it is the right one.
      p[t] = Pigment{rgb(lr, lg, lb), base, rgb(dr, dg, db),
                     cv::Scalar(base[2], base[1], base[0], 255)};
    };
    // In the palette's own order, so the two tables can be diffed.
    set(SAND, 245, 212, 97, 227, 169, 60, 162, 105, 31);    // chrome yellow, wheat
    set(WATER, 127, 178, 232, 44, 95, 200, 22, 38, 94);     // cobalt into ultramarine
    set(SALTWATER, 110, 159, 224, 36, 68, 158, 18, 30, 78);
    set(OIL, 138, 106, 74, 74, 46, 30, 36, 20, 12);         // burnt umber
    set(DIRT, 200, 162, 114, 139, 94, 52, 74, 48, 24);      // burnt sienna
    set(MUD, 138, 106, 69, 90, 58, 32, 46, 30, 14);         // umber
    set(SALT, 251, 246, 230, 232, 223, 198, 169, 156, 124); // lead white
    set(ICE, 242, 251, 254, 191, 230, 240, 111, 168, 196);  // cerulean
    set(PLANT, 143, 196, 106, 63, 122, 70, 30, 66, 39);     // viridian
    set(WALL, 184, 180, 200, 110, 110, 136, 46, 48, 80);    // stone, blue shadow
    set(IRONWALL, 159, 168, 200, 78, 88, 120, 35, 40, 66);  // slate, Prussian
    set(RUST, 208, 128, 80, 154, 74, 32, 82, 36, 14);       // vermilion
    set(TORCH, 255, 208, 138, 255, 138, 42, 176, 58, 14);   // cadmium orange
    set(STOVE, 110, 106, 120, 58, 56, 70, 22, 22, 30);      // ivory black
    set(EMBER, 255, 176, 96, 224, 74, 32, 122, 30, 10);
    set(FIRE, 255, 224, 138, 255, 122, 30, 194, 42, 8);     // cadmium yellow -> orange
    set(ELEC, 255, 255, 208, 255, 232, 58, 200, 168, 0);    // chrome yellow
    set(STEAM, 234, 246, 255, 168, 200, 220, 94, 122, 140);
    set(ACID, 232, 255, 138, 184, 232, 58, 94, 140, 30);    // a sickly chrome green
    set(WATERSPOUT, 143, 192, 240, 46, 98, 168, 20, 46, 92);
    set(SANDSPOUT, 248, 224, 144, 210, 160, 58, 126, 90, 22);
    set(SALTSPOUT, 255, 255, 255, 220, 210, 188, 154, 140, 112);
    set(OILSPOUT, 154, 122, 90, 90, 58, 36, 44, 26, 14);
    set(NIL, 106, 106, 138, 42, 42, 62, 14, 14, 24);        // the void: violet-black
    set(NOTHING, 40, 52, 96, 26, 34, 66, 14, 18, 40);       // the sky under the paint
    return p;
  }();
  return table;
}

inline Pigment pigmentOf(ParticleType t) { return pigmentTable()[t]; }

// ---- the flow of the brush -------------------------------------------------
inline float flowAngle(int cx, int cy, float t) {
  const bool flip = (hashBits(cx, cy) & 1u) != 0u;
  const float dir = flip ? 0.28f : -0.28f;
  return dir + 0.20f * std::sin(float(cy) * 0.050f + t * 0.21f) +
         0.16f * std::cos(float(cx) * 0.041f - t * 0.17f);
}

// ---- one stroke ------------------------------------------------------------
struct Stroke {
  float x0, y0, x1, y1; // the axis of the brush
  float w;              // the loaded width
  float bulge;          // the belly, along the normal
  float wob, wob2;      // the ragged edges, along the normal
};

// Appends a stroke to an already-open path as one closed sub-path.
//
// The direction is canonicalised first. NanoVG fills with a non-zero winding
// rule, so two sub-paths of opposite orientation *subtract* where they overlap:
// a heap of strokes that point both ways would punch holes in itself. Pointing
// every stroke the same way is what makes a pass union instead.
inline void appendStroke(const Stroke &s) {
  using namespace cv::v4d::nvg;
  float ax = s.x0, ay = s.y0, bx = s.x1, by = s.y1;
  if (bx < ax || (bx == ax && by < ay))
    std::swap(ax, bx), std::swap(ay, by);
  const float dx = bx - ax, dy = by - ay;
  const float len = std::sqrt(dx * dx + dy * dy) + 1e-4f;
  const float nx = -dy / len, ny = dx / len;
  const float hw = s.w * 0.5f;
  // The control points sit a third of the way along the axis and are pushed
  // along the normal by the belly, which is what bows the stroke.
  const float c1x = ax + dx * 0.33f + nx * s.bulge;
  const float c1y = ay + dy * 0.33f + ny * s.bulge;
  const float c2x = bx - dx * 0.33f + nx * s.bulge;
  const float c2y = by - dy * 0.33f + ny * s.bulge;
  moveTo(ax - nx * hw + nx * s.wob, ay - ny * hw + ny * s.wob);
  bezierTo(c1x - nx * hw, c1y - ny * hw, c2x - nx * hw, c2y - ny * hw,
           bx - nx * hw + nx * s.wob2, by - ny * hw + ny * s.wob2);
  bezierTo(c2x + nx * hw, c2y + ny * hw, c1x + nx * hw, c1y + ny * hw,
           ax + nx * hw + nx * s.wob, ay + ny * hw + ny * s.wob);
  closePath();
}

// ---- the four passes, batched ---------------------------------------------
// The strokes of one material are collected and filled afterwards, one path
// per (pass, variant). A heap of settled sand is a few thousand strokes, but
// it is still only a handful of draw calls, because NanoVG fills every
// sub-path of a path in one go -- and, with a non-zero winding, the strokes of
// one pass do not darken each other where they overlap.
enum Pass { SHADOW = 0, BODY = 1, LIGHT = 2, ACCENT = 3, kPasses = 4 };
static constexpr int kVariants = 4;

struct Batch {
  std::array<std::array<std::vector<Stroke>, kVariants>, kPasses> pass{};
  void clear() {
    for (auto &p : pass)
      for (auto &v : p)
        v.clear();
  }
};

// The same pigment, pushed four ways: as it is, towards the light, towards the
// dark, and towards its own complement.
inline cv::Scalar variantColor(const Pigment &pig, int variant) {
  switch (variant) {
  case 0:
    return pig.base;
  case 1:
    return mix(pig.base, pig.light, 0.45f);
  case 2:
    return mix(pig.base, pig.dark, 0.45f);
  default:
    return mix(pig.base, pig.comp, 0.55f);
  }
}

// How a material is brushed. The automaton decides where a material is; the
// brush decides how it lies on the canvas.
struct Brush {
  float length = 5.0f;  // cells per stroke
  float width = 0.75f;  // stroke width, in cells
  float alpha = 250.0f; // body alpha
  float flow = 1.0f;   // how tightly the flow field steers the strokes
  float rise = 0.0f;   // extra vertical bias (fire climbs, steam drifts)
  float ground = 250.0f; // the imprimata under the strokes
};

// ---- the painter -----------------------------------------------------------
// One material, over its runs: the ground, then shadow, body, light and the
// accent of the complement, all filled in that order.
void paintMaterial(const Pigment &pig, const vector<Run> &runs, const View &v,
                   float tsec, const Brush &brush, float impasto,
                   Batch &batch) {
  using namespace cv::v4d::nvg;
  if (runs.empty())
    return;
  const float cell = v.cell_;
  batch.clear();

  // --- the ground: one wavy rectangle per run, opaque ----------------------
  {
    beginPath();
    for (const Run &r : runs) {
      const float x0 = v.origin_.x + r.x0_ * cell;
      const float x1 = v.origin_.x + (r.x1_ + 1) * cell;
      const float y0 = v.origin_.y + r.y_ * cell;
      const float y1 = y0 + cell;
      const float mid = (x0 + x1) * 0.5f;
      const float a = hash01(r.x0_, r.y_ + 7) - 0.5f;
      const float b = hash01(r.x0_ + 3, r.y_) - 0.5f;
      moveTo(x0, y0 + cell * 0.12f * a);
      quadTo(mid, y0 + cell * 0.24f * a, x1, y0 + cell * 0.12f * b);
      lineTo(x1, y1 - cell * 0.12f * b);
      quadTo(mid, y1 - cell * 0.22f * a, x0, y1 - cell * 0.12f * a);
      closePath();
    }
    fillColor(withAlpha(pig.base, brush.ground));
    fill();
  }

  // --- the strokes ---------------------------------------------------------
  for (const Run &r : runs) {
    const float x0 = v.origin_.x + r.x0_ * cell;
    const float x1 = v.origin_.x + (r.x1_ + 1) * cell;
    const float yc = v.origin_.y + (r.y_ + 0.5f) * cell;
    const float len = x1 - x0;
    if (len <= 0.25f)
      continue;
    const float cells = float(r.x1_ - r.x0_ + 1);
    const float h = hash01(r.x0_, r.y_);
    const float h2 = hash01(r.y_ + 11, r.x0_);
    // A long run is combed into several strokes; a short one is a single dab.
    const int strokes = std::clamp(
        int(std::floor(len / (cell * brush.length) * impasto + 0.5f)), 1, 4);
    const float w = cell * brush.width * (0.58f + 0.42f * h);
    for (int s = 0; s < strokes; ++s) {
      const float f0 = (float(s) + 0.18f) / float(strokes);
      const float f1 = (float(s) + 0.82f) / float(strokes);
      const float mid = x0 + len * (f0 + f1) * 0.5f;
      const int cx = int(float(r.x0_) + cells * (f0 + f1) * 0.5f);
      float ang = flowAngle(cx, r.y_, tsec) * brush.flow;
      ang += brush.rise;
      const float ca = std::cos(ang), sa = std::sin(ang);
      const float nx = -sa, ny = ca;
      const float half = std::max(len / float(strokes) * 0.42f, cell * 0.25f);
      const Stroke axis{mid - ca * half, yc - sa * half, mid + ca * half,
                        yc + sa * half, w, (h2 - 0.5f) * w * 1.4f,
                        (h - 0.5f) * cell * 0.16f, (h2 - 0.5f) * cell * 0.16f};
      const int variant =
          int(hash01(r.x0_ + s * 31, r.y_ + 5) * float(kVariants + 1)) %
          kVariants;

      // The underpainting: the shadow the stroke throws on what is below it.
      batch.pass[SHADOW][variant].push_back(
          Stroke{axis.x0 + nx * cell * 0.20f, axis.y0 + ny * cell * 0.20f,
                 axis.x1 + nx * cell * 0.20f, axis.y1 + ny * cell * 0.20f,
                 w * 1.10f, axis.bulge, axis.wob, axis.wob2});
      // The body.
      batch.pass[BODY][variant].push_back(axis);
      // The ridge of thick paint that catches the light.
      batch.pass[LIGHT][variant].push_back(
          Stroke{axis.x0 - nx * cell * 0.12f, axis.y0 - ny * cell * 0.12f,
                 axis.x1 - nx * cell * 0.12f, axis.y1 - ny * cell * 0.12f,
                 w * 0.40f, axis.bulge, axis.wob * 0.5f, axis.wob2 * 0.5f});
      // And one thin note of the complement along the stroke's far edge, laid
      // on only when there is paint to spare.
      if (impasto >= 1.0f)
        batch.pass[ACCENT][variant].push_back(
            Stroke{axis.x0 + nx * cell * 0.32f, axis.y0 + ny * cell * 0.32f,
                   axis.x1 + nx * cell * 0.32f, axis.y1 + ny * cell * 0.32f,
                   cell * 0.16f, axis.bulge, 0.0f, 0.0f});
    }
  }

  // --- fill the four passes, dark to light ---------------------------------
  for (int pass = 0; pass < kPasses; ++pass) {
    for (int variant = 0; variant < kVariants; ++variant) {
      const auto &strokes = batch.pass[pass][variant];
      if (strokes.empty())
        continue;
      cv::Scalar col = pig.base;
      float alpha = brush.alpha;
      switch (pass) {
      case SHADOW:
        col = mix(pig.dark, pig.comp, 0.22f);
        alpha = 118.0f;
        break;
      case BODY:
        col = variantColor(pig, variant);
        alpha = brush.alpha;
        break;
      case LIGHT:
        col = mix(pig.light, cream(), 0.35f);
        alpha = 138.0f;
        break;
      default:
        col = pig.comp;
        alpha = 96.0f;
        break;
      }
      beginPath();
      for (const Stroke &s : strokes)
        appendStroke(s);
      fillColor(withAlpha(col, alpha));
      fill();
    }
  }
}

// ---- the glare -------------------------------------------------------------
// The other half of the look: light that does not merely reflect but blooms
// out of the canvas. NanoVG's NVG_LIGHTER composites additively, which is
// exactly what a lit oil surface does -- Van Gogh's fire has no edge.
void glare(const cv::Scalar &col, float cx, float cy, float r, float alpha) {
  using namespace cv::v4d::nvg;
  if (r <= 0.0f || alpha <= 0.0f)
    return;
  beginPath();
  circle(cx, cy, r);
  fillPaint(radialGradient(cx, cy, 0.0f, r, withAlpha(col, alpha),
                           cv::Scalar(0, 0, 0, 0)));
  fill();
}

void glareRun(const cv::Scalar &col, const vector<Run> &runs, const View &v,
              float radius, float alpha, float tsec) {
  using namespace cv::v4d::nvg;
  save();
  globalCompositeOperation(NVG_LIGHTER);
  for (const Run &r : runs) {
    // A hash of the cell and the frame *quantum* rather than a re-roll every
    // frame: fire flickers at 12 Hz, smoothly, instead of boiling.
    const float phase = tsec * 12.0f;
    const float q = std::floor(phase);
    const float k = phase - q;
    const float a = 0.72f + 0.28f * hash01(r.x0_ * 2 + int(q), r.y_);
    const float b = 0.72f + 0.28f * hash01(r.x0_ * 2 + int(q) + 1, r.y_);
    const float flick = a + (b - a) * k;
    glare(col, v.origin_.x + (r.x0_ + r.x1_ + 1) * 0.5f * v.cell_,
          v.origin_.y + (r.y_ + 0.5f) * v.cell_, radius, alpha * flick);
  }
  restore();
}

// ---- the canvas ------------------------------------------------------------
// A tileable linen weave, generated once and uploaded as one NanoVG image:
// two families of coarse thread running across each other, with the fibre
// noise of real cloth. It is the difference between a painting on canvas and a
// picture on a screen, and it costs one image fill.
int weaveImage() {
  // One per process. The node that reads it executes on a single worker
  // thread, and the image lives in the NanoVG context, not in the CPU cache.
  static int handle = -1;
  if (handle < 0) {
    constexpr int N = 256;
    cv::Mat weave(N, N, CV_8UC4);
    for (int y = 0; y < N; ++y) {
      for (int x = 0; x < N; ++x) {
        // Periods of 64 px, four times over the tile, so the weave repeats.
        const float th = 0.5f + 0.5f * std::sin(float(y) * 0.0981748f);
        const float tv = 0.5f + 0.5f * std::sin(float(x) * 0.0981748f + 1.7f);
        // Fibre noise on a 16 px lattice, also periodic over the tile.
        const float n = hash01(x & 15, y & 15);
        const float shade = 0.30f * th + 0.30f * tv + 0.40f * n;
        weave.at<cv::Vec4b>(y, x) =
            cv::Vec4b(232, 224, 205, uint8_t(std::clamp(shade, 0.0f, 1.0f) *
                                             150.0f));
      }
    }
    handle = cv::v4d::nvg::createImageRGBA(
        N, N, NVG_IMAGE_REPEATX | NVG_IMAGE_REPEATY, weave.data);
  }
  return handle;
}

// The canvas, over a rectangle: the weave at a given strength.
void paintCanvas(float x, float y, float w, float h, float strength) {
  using namespace cv::v4d::nvg;
  if (w <= 0.0f || h <= 0.0f)
    return;
  const int img = weaveImage();
  if (img < 0)
    return;
  beginPath();
  rect(x, y, w, h);
  fillPaint(imagePattern(x, y, x + w, y + h, 0.0f, img, strength));
  fill();
}

// ---- the night -------------------------------------------------------------
// Starry Night, as the empty cells of the world: a field of ultramarine with
// three vortices rolled through it and eleven halated stars. Van Gogh painted
// eleven -- ten stars and the moon -- so eleven it is.
void paintNight(float x, float y, float w, float h, float cell, float tsec) {
  using namespace cv::v4d::nvg;

  // The field: a vertical wash from Prussian blue at the top of the slab to
  // near-black at the bottom.
  beginPath();
  rect(x, y, w, h);
  fillPaint(linearGradient(x, y, x, y + h, cv::Scalar(150, 104, 46, 255),
                           cv::Scalar(40, 26, 12, 255)));
  fill();

  // Three vortices, rolled as short spiral ribbons of cerulean.
  static const float kCol[3] = {0.24f, 0.62f, 0.86f};
  static const float kRow[3] = {0.30f, 0.68f, 0.44f};
  for (int k = 0; k < 3; ++k) {
    const float vx = x + w * kCol[k], vy = y + h * kRow[k];
    const float R = std::min(w, h) * (0.18f + 0.09f * k);
    const float spin = 0.05f + 0.02f * float(k);
    for (int i = 0; i < 18; ++i) {
      const float t = float(i) / 18.0f;
      // The golden angle: each segment starts where the last left off, which is
      // what makes a spiral rather than a wreath.
      const float a0 = 2.399963f * float(i) + tsec * spin;
      const float r0 = R * (0.12f + 0.88f * t);
      const float r1 = R * (0.12f + 0.88f * (t + 0.07f));
      const float a1 = a0 + 0.62f;
      Stroke s;
      s.x0 = vx + r0 * std::cos(a0);
      s.y0 = vy + r0 * std::sin(a0);
      s.x1 = vx + r1 * std::cos(a1);
      s.y1 = vy + r1 * std::sin(a1);
      s.w = std::max(1.5f, cell * (0.45f + 0.75f * t));
      s.bulge = R * 0.06f * std::sin(a0 * 2.0f);
      s.wob = (hash01(i, k) - 0.5f) * s.w * 0.3f;
      s.wob2 = (hash01(k, i) - 0.5f) * s.w * 0.3f;
      beginPath();
      appendStroke(s);
      fillColor(withAlpha(mix(cerulean(), cream(), 0.30f + 0.45f * t),
                          44.0f + 26.0f * t));
      fill();
    }
  }

  // Eleven stars, each a core, a halo and seven rays.
  for (int i = 0; i < 11; ++i) {
    const float sx = x + w * (0.05f + 0.90f * hash01(i, 101));
    const float sy = y + h * (0.04f + 0.38f * hash01(i, 202));
    const float rad = cell * (4.5f + 9.0f * hash01(i, 303));
    const float tw = 0.55f + 0.45f * std::sin(tsec * 1.6f + float(i) * 2.1f);
    save();
    globalCompositeOperation(NVG_LIGHTER);
    glare(mix(cream(), cv::Scalar(30, 170, 235, 255), 0.45f), sx, sy, rad * 2.2f,
          140.0f * tw);
    glare(cream(), sx, sy, rad * 0.55f, 235.0f * tw);
    constexpr int kRays = 7;
    for (int k = 0; k < kRays; ++k) {
      const float a = 6.2831853f * float(k) / float(kRays) + float(i);
      Stroke s;
      s.x0 = sx + std::cos(a) * rad * 0.5f;
      s.y0 = sy + std::sin(a) * rad * 0.5f;
      s.x1 = sx + std::cos(a) * rad * 1.9f;
      s.y1 = sy + std::sin(a) * rad * 1.9f;
      s.w = std::max(1.0f, rad * 0.16f);
      s.bulge = 0.0f;
      s.wob = 0.0f;
      s.wob2 = 0.0f;
      beginPath();
      appendStroke(s);
      fillColor(withAlpha(cream(), 110.0f * tw));
      fill();
    }
    restore();
  }
}

// ---- the varnish -----------------------------------------------------------
// The last thing on the canvas: a vignette, the way a painting darkens at the
// edges where the light does not fall straight on it.
void paintVignette(const cv::Size &sz, float strength) {
  using namespace cv::v4d::nvg;
  const float cx = sz.width * 0.5f, cy = sz.height * 0.5f;
  const float r = std::sqrt(cx * cx + cy * cy) + 1.0f;
  beginPath();
  rect(0.0f, 0.0f, float(sz.width), float(sz.height));
  fillPaint(radialGradient(cx, cy, r * 0.42f, r * 1.06f, cv::Scalar(0, 0, 0, 0),
                           withAlpha(cv::Scalar(10, 7, 3, 255), strength)));
  fill();
}

} // namespace vg

// GL_CHECK in <v4d/util.hpp> wraps a *single* expression and chokes on the
// commas of any multi-argument GL call (glDrawArrays, glVertexAttribPointer,
// glTexImage2D, the glBlitFramebuffer ...). Redefine it here, variadic, so it
// wraps calls of any arity while still checking for GL errors in debug builds.
// Only VanGoghPost below uses GL_CHECK in this translation unit.
#undef GL_CHECK
#ifdef NDEBUG
#define GL_CHECK(...) \
  do {                \
    __VA_ARGS__;      \
  } while (0)
#else
#define GL_CHECK(...)                                                     \
  do {                                                                    \
    __VA_ARGS__;                                                          \
    cv::v4d::gl_check_error(__FILE__, __LINE__, #__VA_ARGS__);            \
  } while (0)
#endif

// ===========================================================================
// VanGoghPost -- the varnish and the light, done in raw GL (never fb())
// ===========================================================================
//
// The painting is drawn with nvg() into the framebuffer; this node then reads
// that framebuffer back, lets luminous paint bloom out of it, softens the whole
// into velvety impasto with a living swirl, and lays a lit-from-the-room
// vignette over it -- and never touches fb(). The trick is that a gl() node
// runs against the same texture-backed framebuffer nvg() just filled, so
// discovering it with glGetIntegerv(GL_FRAMEBUFFER_BINDING), blitting it into a
// texture of our own, and painting a full-screen quad back into it is all plain
// GL: correct on desktop GL 3.2 core and on the OpenGL ES 3.0 path, where the
// shader below compiles unchanged (the #version line is prepended by
// cv::v4d::init_shaders for whichever backend is in use).
struct VanGoghPost {
  GLuint quadVAO_ = 0;
  GLuint quadVBO_ = 0;
  GLuint program_ = 0;
  GLuint sceneFBO_ = 0;
  GLuint sceneTex_ = 0;
  cv::Size texSize_;
  GLint uTex_ = -1, uRes_ = -1, uTexel_ = -1, uTime_ = -1, uPaint_ = -1,
        uGlow_ = -1, uBrush_ = -1, uSwirl_ = -1, uVig_ = -1, uGrade_ = -1;

  // A full-screen triangle-strip. The UV layout matches the runtime's own blit
  // quad, so sampling and writing are a 1:1 texel copy of whatever the nvg()
  // base left behind -- the effect never rotates or mirrors the painting.
  static constexpr float kQuad_[16] = {// pos       uv
                                       -1.0f, 1.0f,  0.0f, 1.0f, -1.0f, -1.0f,
                                       0.0f,  0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                       1.0f,  -1.0f, 1.0f, 0.0f};

  static const char *vertSrc() {
    return R"(
			precision highp float;
			layout(location = 0) in vec2 aPos;
			layout(location = 1) in vec2 aUV;
			out vec2 vUV;
			void main() {
				vUV = aUV;
				gl_Position = vec4(aPos, 0.0, 1.0);
			}
		)";
  }

  static const char *fragSrc() {
    return R"(
			precision highp float;

			in vec2 vUV;
			out vec4 fragColor;

			uniform sampler2D uTex;
			uniform vec2  uResolution;
			uniform vec2  uTexel;
			uniform float uTime;
			uniform float uPaint;
			uniform float uGlow;
			uniform float uBrush;
			uniform float uSwirl;
			uniform float uVignette;
			uniform vec3  uGrade;

			float hash21(vec2 p) {
				p = fract(p * vec2(123.34, 456.21));
				p += dot(p, p + 45.32);
				return fract(p.x * p.y);
			}

			float vnoise(vec2 p) {
				vec2 i = floor(p);
				vec2 f = fract(p);
				float a = hash21(i);
				float b = hash21(i + vec2(1.0, 0.0));
				float c = hash21(i + vec2(0.0, 1.0));
				float d = hash21(i + vec2(1.0, 1.0));
				vec2 u = f * f * (3.0 - 2.0 * f);
				return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
			}

			float fbm(vec2 p) {
				float s = 0.0;
				float a = 0.5;
				for (int i = 0; i < 4; ++i) {
					s += a * vnoise(p);
					p *= 2.03;
					a *= 0.5;
				}
				return s;
			}

			// Anisotropic-Kuwahara-style soften: over a 3x3 neighbourhood take the
			// corner quadrant with the least colour variance, so interiors melt
			// into velvety paint while the brightest edges stay put. Used gently,
			// it is a varnish that unifies the strokes rather than erases them.
			vec3 oilPaint(vec2 uv) {
				vec2 t = uTexel;
				vec3 sum[4];
				vec3 sumSq[4];
				for (int q = 0; q < 4; ++q) {
					vec2 corner = (q == 0) ? vec2(-1.0, 1.0) :
					               (q == 1) ? vec2(1.0, 1.0) :
					               (q == 2) ? vec2(-1.0, -1.0) : vec2(1.0, -1.0);
					vec2 base = corner * t;
					vec3 s = vec3(0.0);
					vec3 sq = vec3(0.0);
					for (int j = 0; j < 2; ++j) {
						for (int k = 0; k < 2; ++k) {
							vec3 c = texture(uTex, uv + base + vec2(float(j) * t.x, float(k) * t.y)).rgb;
							s += c;
							sq += c * c;
						}
					}
					sum[q] = s / 4.0;
					sumSq[q] = sq / 4.0;
				}
				float bestVar = 1.0e9;
				vec3 best = sum[0];
				for (int q = 0; q < 4; ++q) {
					vec3 v = max(sumSq[q] - sum[q] * sum[q], vec3(0.0));
					float var = v.x + v.y + v.z;
					if (var < bestVar) { bestVar = var; best = sum[q]; }
				}
				return best;
			}

			// The light that does not stop at the edge of a material: two offset
			// rings weighted by luminance, so fire, sparks and the painted stars
			// throw out the halo Van Gogh gave them across the whole painting.
			vec3 bloom(vec2 uv) {
				vec3 acc = vec3(0.0);
				float weight = 0.0;
				for (int i = 0; i < 8; ++i) {
					float ang = 6.2831853 * float(i) / 8.0;
					vec3 c = texture(uTex, uv + vec2(cos(ang), sin(ang)) * uTexel * 4.0).rgb;
					float lum = dot(c, vec3(0.299, 0.587, 0.114));
					float w = smoothstep(0.55, 1.0, lum);
					acc += c * w; weight += w;
				}
				for (int i = 0; i < 8; ++i) {
					float ang = 6.2831853 * float(i) / 8.0 + 0.4;
					vec3 c = texture(uTex, uv + vec2(cos(ang), sin(ang)) * uTexel * 11.0).rgb;
					float lum = dot(c, vec3(0.299, 0.587, 0.114));
					float w = smoothstep(0.48, 1.0, lum) * 0.7;
					acc += c * w; weight += w;
				}
				return acc / max(weight, 1.0e-3);
			}

			void main() {
				vec2 uv = vUV;

				// living paint: a slow swirl drifts the sample along a noise field
				vec2 flow = vec2(fbm(uv * 3.0 + vec2(uTime * 0.03, 0.0)),
				                 fbm(uv * 3.0 + vec2(0.0, uTime * 0.03) + 17.3)) - 0.5;
				uv += flow * uSwirl;

				vec3 col = texture(uTex, uv).rgb;
				col = mix(col, oilPaint(uv), clamp(uPaint, 0.0, 1.0));
				col += bloom(uv) * uGlow;

				// a faint bristle sheen along the flow, as if raked light crossed
				// the ridges of the paint
				float bristle = fbm(vec2((uv.x + flow.x * 0.15) * (uResolution.x * 0.08),
				                         uv.y * 4.0));
				col *= mix(1.0, 0.93 + 0.14 * bristle, uBrush);

				// warm Van Gogh grade, then a lit-from-the-room vignette
				col *= uGrade;
				col = pow(max(col, vec3(0.0)), vec3(0.96, 1.0, 1.06));
				vec2 d = uv - 0.5;
				col *= clamp(1.0 - uVignette * dot(d, d) * 1.6, 0.0, 1.0);

				fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
			}
		)";
  }

  void makeProgram() {
    unsigned int handles[3];
    cv::v4d::init_shaders(handles, vertSrc(), fragSrc(), "fragColor");
    program_ = handles[0];
    uTex_ = glGetUniformLocation(program_, "uTex");
    uRes_ = glGetUniformLocation(program_, "uResolution");
    uTexel_ = glGetUniformLocation(program_, "uTexel");
    uTime_ = glGetUniformLocation(program_, "uTime");
    uPaint_ = glGetUniformLocation(program_, "uPaint");
    uGlow_ = glGetUniformLocation(program_, "uGlow");
    uBrush_ = glGetUniformLocation(program_, "uBrush");
    uSwirl_ = glGetUniformLocation(program_, "uSwirl");
    uVig_ = glGetUniformLocation(program_, "uVignette");
    uGrade_ = glGetUniformLocation(program_, "uGrade");
  }

  // Create the offscreen texture + FBO when first needed and on every resize.
  void ensureSize(int w, int h) {
    if (sceneFBO_ != 0 && texSize_ == cv::Size(w, h))
      return;
    if (sceneTex_) {
      glDeleteTextures(1, &sceneTex_);
      sceneTex_ = 0;
    }
    if (sceneFBO_) {
      glDeleteFramebuffers(1, &sceneFBO_);
      sceneFBO_ = 0;
    }
    texSize_ = cv::Size(w, h);
    GL_CHECK(glGenTextures(1, &sceneTex_));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, sceneTex_));
    GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                          GL_UNSIGNED_BYTE, nullptr));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    GL_CHECK(glGenFramebuffers(1, &sceneFBO_));
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO_));
    GL_CHECK(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_TEXTURE_2D, sceneTex_, 0));
  }

  // Runs inside a gl() node on the worker's own GL context.
  void process() {
    GLint prevFBO = 0;
    GL_CHECK(glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO));
    GLint vp[4] = {0, 0, 0, 0};
    GL_CHECK(glGetIntegerv(GL_VIEWPORT, vp));
    const int W = vp[2], H = vp[3];
    if (W <= 0 || H <= 0)
      return;
    ensureSize(W, H);

    // Capture the base nvg left behind into our own texture (a plain colour
    // blit, no fb()).
    GL_CHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevFBO));
    GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneFBO_));
    GL_CHECK(
        glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST));

    // Paint the treated image back into the same framebuffer in one quad.
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO));
    GL_CHECK(glViewport(0, 0, W, H));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);

    GL_CHECK(glUseProgram(program_));
    GL_CHECK(glUniform1i(uTex_, 0));
    GL_CHECK(glUniform2f(uRes_, (float)W, (float)H));
    GL_CHECK(glUniform2f(uTexel_, 1.0f / (float)W, 1.0f / (float)H));
    GL_CHECK(glUniform1f(uTime_, (float)cv::v4d::seconds()));
    GL_CHECK(glUniform1f(uPaint_, 0.42f));
    GL_CHECK(glUniform1f(uGlow_, 0.95f));
    GL_CHECK(glUniform1f(uBrush_, 0.5f));
    GL_CHECK(glUniform1f(uSwirl_, 0.0045f));
    GL_CHECK(glUniform1f(uVig_, 0.55f));
    GL_CHECK(glUniform3f(uGrade_, 1.03f, 1.0f, 0.94f));
    GL_CHECK(glActiveTexture(GL_TEXTURE0));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, sceneTex_));
    GL_CHECK(glBindVertexArray(quadVAO_));
    GL_CHECK(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));

    // Leave GL the way we found it; the overlay nvg node sets its own state.
    GL_CHECK(glBindVertexArray(0));
    GL_CHECK(glUseProgram(0));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO));
    glEnable(GL_BLEND);
  }

  void init() {
    GL_CHECK(glGenVertexArrays(1, &quadVAO_));
    GL_CHECK(glGenBuffers(1, &quadVBO_));
    GL_CHECK(glBindVertexArray(quadVAO_));
    GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, quadVBO_));
    GL_CHECK(glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad_), kQuad_,
                          GL_STATIC_DRAW));
    GL_CHECK(glEnableVertexAttribArray(0));
    GL_CHECK(glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                                   (void *)0));
    GL_CHECK(glEnableVertexAttribArray(1));
    GL_CHECK(glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                                   (void *)(2 * sizeof(float))));
    GL_CHECK(glBindVertexArray(0));
    GL_CHECK(glBindBuffer(GL_ARRAY_BUFFER, 0));
    makeProgram();
  }

  void destroy() {
    if (sceneTex_) {
      glDeleteTextures(1, &sceneTex_);
      sceneTex_ = 0;
    }
    if (sceneFBO_) {
      glDeleteFramebuffers(1, &sceneFBO_);
      sceneFBO_ = 0;
    }
    if (program_) {
      glDeleteProgram(program_);
      program_ = 0;
    }
    if (quadVBO_) {
      glDeleteBuffers(1, &quadVBO_);
      quadVBO_ = 0;
    }
    if (quadVAO_) {
      glDeleteVertexArrays(1, &quadVAO_);
      quadVAO_ = 0;
    }
    texSize_ = cv::Size();
  }
};

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

// The brush a material is laid on with, in Van Gogh's hand: how long a stroke
// runs, how wide it is loaded, how tightly it follows the field, and how much
// it leans out of the horizontal. Solids are combed into the field; liquids
// are laid in long horizontal dashes; fire and steam are steep and hot; acid
// is a stain rather than a body.
static vg::Brush brushOf(ParticleType type) {
  switch (materialOf(type)) {
  case Material::EMBERISH:
    return {3.5f, 0.55f, 255.0f, 0.6f, 1.25f, 250.0f};
  case Material::VAPOUR:
    return {3.0f, 0.60f, 150.0f, 0.8f, 1.10f, 70.0f};
  case Material::LIQUID:
    return {7.0f, 0.80f, 210.0f, 1.0f, 0.0f, 240.0f};
  case Material::DISSOLVE:
    return {4.5f, 0.70f, 225.0f, 0.7f, 0.0f, 130.0f};
  default:
    return {5.0f, 0.75f, 250.0f, 1.0f, 0.0f, 250.0f};
  }
}

static void drawMaterial(const Palette &pal, ParticleType type,
                         const vector<Run> &runs, const View &v,
                         float timeSec, bool vanGogh, float impasto) {
  using namespace cv::v4d::nvg;
  if (runs.empty())
    return;
  const cv::Scalar base = pal[type];
  const float cell = v.cell_;

  // --- the painter's path ---------------------------------------------------
  //
  // Below roughly two and a half pixels a cell there is no room for a stroke:
  // the brush becomes a smear and the world becomes mud. Small windows
  // therefore fall through to the band renderer below, which still paints in
  // the pigments rather than the palette.
  if (vanGogh && cell >= 2.6f) {
    static thread_local vg::Batch batch;
    const vg::Pigment pig = vg::pigmentOf(type);
    const vg::Brush brush = brushOf(type);

    vg::paintMaterial(pig, runs, v, timeSec, brush, impasto, batch);

    switch (materialOf(type)) {
    case Material::EMBERISH:
      // Fire, embers, electricity and torches: light that does not stop at the
      // edge of the material. Additive, and flickering at 12 Hz -- a hash of
      // the cell and the frame quantum, so the flame lives without boiling.
      vg::glareRun(vg::mix(pig.light, vg::cream(), 0.25f), runs, v, cell * 1.7f,
                   96.0f, timeSec);
      break;
    case Material::LIQUID: {
      // The surface of Van Gogh's water is not one colour but a field of small
      // bright dashes laid over the blue and drifting with the current.
      save();
      globalCompositeOperation(NVG_LIGHTER);
      beginPath();
      for (const Run &r : runs) {
        const float len = (r.x1_ - r.x0_ + 1) * cell;
        if (len < cell * 1.5f)
          continue;
        const int n = std::clamp(int(len / (cell * 3.0f)), 1, 8);
        for (int k = 0; k < n; ++k) {
          const float f = (float(k) + 0.5f) / float(n);
          const float drift = std::sin(timeSec * 1.3f + float(r.y_) * 0.7f +
                                       f * 6.2831853f) * cell * 0.25f;
          const float cx =
              v.origin_.x + (float(r.x0_) + (r.x1_ - r.x0_ + 1) * f) * cell;
          const float cy = v.origin_.y + (r.y_ + 0.38f) * cell + drift;
          vg::appendStroke(vg::Stroke{cx - cell * 0.55f, cy, cx + cell * 0.55f,
                                      cy, std::max(1.0f, cell * 0.18f), 0.0f,
                                      0.0f, 0.0f});
        }
      }
      fillColor(vg::withAlpha(vg::mix(pig.light, vg::cream(), 0.55f), 150.0f));
      fill();
      restore();
      break;
    }
    default:
      break;
    }
    return;
  }

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
//
// Two styles, one function. The band path is the original: it is where small
// windows fall back to (below ~2.6 px a cell there is no room for a stroke),
// and where the panel's checkbox returns to. The painter's path stages the
// scene instead -- linen on the studio wall, a gilded frame, a night the world
// hangs in, the world as strokes over the stars, the canvas weave once more
// over the paint and the varnish last.
static void drawWorld(const Palette &pal, Sandbox &sb, const View &v,
                      const cv::Size &sz, double timeSec) {
  using namespace cv::v4d::nvg;
  const float cell = v.cell_;
  const float w = cell * v.width_;
  const float h = cell * v.height_;
  const cv::Point2f o = v.origin_;
  const bool vg = sb.vanGogh_;
  const float tsec = float(timeSec);
  const cv::Scalar cream = vg::cream();

  // ---- the studio wall -----------------------------------------------------
  if (vg) {
    // Linen over the whole window, well under the paint: it should be felt
    // rather than seen, and a weave that competes with the brushwork is a
    // texture, not a canvas.
    vg::paintCanvas(0.0f, 0.0f, float(sz.width), float(sz.height), 0.055f);
  }

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

    if (vg) {
      // A frame, not a tray: an ivory-black ground under three passes of the
      // chrome yellow the painting itself is made of, so the line reads as
      // gilding laid on with a knife rather than printed around the edge.
      beginPath();
      roundedRect(o.x - 7.0f, o.y - 7.0f, w + 14.0f, h + 14.0f, 8.0f);
      fillColor(cv::Scalar(16, 12, 6, 255));
      fill();

      const cv::Scalar gold(60, 169, 231, 255);
      beginPath();
      roundedRect(o.x - 4.5f, o.y - 4.5f, w + 9.0f, h + 9.0f, 6.0f);
      strokeColor(vg::withAlpha(gold, 90.0f));
      strokeWidth(3.2f);
      stroke();

      beginPath();
      roundedRect(o.x - 3.5f, o.y - 3.5f, w + 7.0f, h + 7.0f, 5.0f);
      strokeColor(vg::withAlpha(gold, 170.0f));
      strokeWidth(1.4f);
      stroke();

      beginPath();
      roundedRect(o.x - 2.0f, o.y - 2.0f, w + 4.0f, h + 4.0f, 6.0f);
      strokeColor(vg::withAlpha(vg::mix(gold, cream, 0.25f), 215.0f));
      strokeWidth(1.0f);
      stroke();
    } else {
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
  }

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

    if (vg) {
      // The nozzle as a loaded brush: one flat dab for the stem, one for the
      // tip, and a halo of the light pouring out of it.
      const vg::Pigment pig = vg::pigmentOf(e.type);
      const vg::Stroke stem{cx - cell * 0.8f, cy - cell * 1.05f, cx + cell * 0.8f,
                            cy - cell * 1.05f, cell * 0.85f, 0.0f, 0.0f, 0.0f};
      const vg::Stroke tip{cx - cell * 0.44f, cy + cell * 0.02f,
                           cx + cell * 0.44f, cy + cell * 0.02f, cell * 0.55f,
                           0.0f, 0.0f, 0.0f};
      beginPath();
      vg::appendStroke(stem);
      fillColor(vg::withAlpha(pig.dark, 235.0f));
      fill();
      beginPath();
      vg::appendStroke(tip);
      fillColor(vg::withAlpha(e.on ? pig.base : pig.dark, 245.0f));
      fill();
      if (e.on) {
        const float glow = 0.5f + 0.5f * std::sin(tsec * 4.0f + float(i));
        vg::glare(vg::mix(pig.light, cream, 0.40f), cx, cy - cell * 0.5f,
                  cell * 1.4f, 64.0f + 56.0f * glow);
      }
    } else {
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
    }

    // The name goes clear of the nozzle rather than on top of it: the nozzle
    // reaches cell*1.1 above the mouth, so the label's baseline has to sit
    // above that or the two overlap at small cell sizes.
    cv::Scalar label = col;
    label[3] = e.on ? 230.0f : 90.0f;
    if (vg)
      fillColor(vg::withAlpha(e.on ? cream : vg::mix(cream, label, 0.4f),
                              e.on ? 235.0f : 110.0f));
    else
      fillColor(label);
    text(cx, cy - cell * 1.1f - 3.0f, e.name, e.name + std::strlen(e.name));
  }

  // --- what the world is painted on, and over ------------------------------
  if (vg) {
    // Starry Night, as the empty cells: the night is drawn first and the world
    // is painted over it, so every pile hangs in a sky rather than on a tray.
    scissor(o.x, o.y, w, h);
    vg::paintNight(o.x, o.y, w, h, cell, tsec);
  } else {
    // The empty field, drawn once so the run pass only has to cover what is
    // actually occupied.
    beginPath();
    rect(o.x, o.y, w, h);
    fillColor(cv::Scalar(16, 16, 22, 255));
    fill();
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
    drawMaterial(pal, t, runs[t], v, tsec, vg, sb.impasto_);

  if (vg) {
    // The canvas the world was painted on: the weave once more, over the slab
    // only, where the paint is. A painting shows its cloth most where the
    // paint is thickest, and at a higher strength than on the studio wall.
    vg::paintCanvas(o.x, o.y, w, h, 0.11f);
    resetScissor();
  }
}

// The chrome that stays crisp: the brush ring and the HUD. It is drawn on top
// of the finished painting -- after the gl() varnish pass -- so the cursor and
// the readouts stay legible over the paint rather than melting into it.
static void drawChrome(const Palette &pal, Sandbox &sb, const View &v,
                       const cv::Size &sz) {
  using namespace cv::v4d::nvg;
  const float cell = v.cell_;
  const float w = cell * v.width_;
  const float h = cell * v.height_;
  const cv::Point2f o = v.origin_;
  const bool vg = sb.vanGogh_;
  const cv::Scalar cream = vg::cream();

  // --- the brush ring ---
  if (sb.penCell_.x >= 0.0 && cell > 0.75f) {
    const float r = (sb.penSize_ + 0.5f) * cell;
    const float cx = o.x + (sb.penCell_.x + 0.5f) * cell;
    const float cy = o.y + (sb.penCell_.y + 0.5f) * cell;
    const cv::Scalar col = pal[sb.brush_];
    if (vg) {
      const vg::Pigment pig = vg::pigmentOf(sb.brush_);
      vg::glare(vg::mix(pig.base, cream, 0.20f), cx, cy, r * 1.7f, 56.0f);
      // A painted ring, not a drawn one: the circle stippled into short
      // arc-strokes, a dark one underneath and a cream one on top, so the
      // cursor has the same hand-made edge as everything else in the scene.
      const int segs = 16;
      lineCap(NVG_ROUND);
      for (int passI = 0; passI < 2; ++passI) {
        const bool under = passI == 0;
        beginPath();
        for (int i = 0; i < segs; ++i) {
          const float a0 = 6.2831853f * float(i) / float(segs);
          const float a1 = a0 + 6.2831853f * 0.60f / float(segs);
          moveTo(cx + std::cos(a0) * r, cy + std::sin(a0) * r);
          arc(cx, cy, r, a0, a1, NVG_CW);
        }
        strokeColor(under ? vg::withAlpha(vg::pigmentOf(NIL).dark, 195.0f)
                          : vg::withAlpha(cream, 215.0f));
        strokeWidth(under ? std::max(2.6f, cell * 0.16f)
                          : std::max(1.2f, cell * 0.07f));
        stroke();
      }
    } else {
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
  }

  // --- the HUD, in the framebuffer's own coordinates and in the footer band ---
  //
  // Anchored to the foot of the reserved band rather than to a fixed offset
  // below the slab: `o.y + h` is the bottom of the world, and a stats line
  // placed a constant distance under it lands outside the framebuffer on any
  // window short enough that the fit leaves less room than that distance.
  const float hudTop = o.y + h + 6.0f;
  // Cream over umber, with a dark passage a pixel underneath: the same trick as
  // the brush ring. A flat fill over a weave is unreadable at this size.
  const auto paintText = [&](const char *s, const char *e, float x, float y,
                             const cv::Scalar &col) {
    if (vg) {
      fillColor(vg::withAlpha(vg::pigmentOf(NIL).dark, 205.0f));
      text(x + 1.0f, y + 1.0f, s, e);
    }
    fillColor(col);
    text(x, y, s, e);
  };
  fontFace("sans-bold");
  fontSize(16.0f);
  textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
  const string title = "Sandcraft";
  paintText(title.c_str(), title.c_str() + title.size(), o.x, hudTop,
            vg ? cream : cv::Scalar(225, 225, 240, 235));

  fontFace("sans");
  fontSize(12.5f);
  char stats[256];
  std::snprintf(stats, sizeof(stats),
                "%d particles   %.0f fps   %dx%d cells   %s, size %d",
                sb.particleCount_, sb.fps_, sb.width_, sb.height_,
                brushName(sb.brush_), sb.penSize_);
  paintText(stats, stats + std::strlen(stats), o.x, hudTop + 20.0f,
            vg ? vg::withAlpha(cream, 215.0f) : cv::Scalar(150, 150, 170, 230));

  textAlign(NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
  constexpr const char *kHint =
      "drag to paint   wheel: brush   SHIFT+wheel: pen   DELETE: clear";
  paintText(kHint, kHint + std::strlen(kHint), sz.width - o.x, hudTop + 20.0f,
            vg ? vg::withAlpha(cream, 165.0f) : cv::Scalar(120, 120, 140, 200));

  // The varnish -- the lit-from-the-room vignette -- is not chrome: the gl()
  // node lays it over the painting, under this overlay, so it darkens the paint
  // and not the readouts.
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

          // ---- the studio: the panel dresses as the painting does -----------
          //
          // Prussian blue ground, chrome-yellow rules, cream lettering, and
          // selection in the yellow of the wheat. The module's own NanoVG
          // context is where the world is painted; the panel is ImGui and
          // therefore styled through the style stack, pushed once for the
          // duration of both windows and popped after the last End().
          const bool styled = sb.vanGogh_;
          int pushedColors = 0;
          int pushedVars = 0;
          if (styled) {
            const auto push = [&](ImGuiCol_ idx, ImVec4 c) {
              PushStyleColor(idx, c);
              ++pushedColors;
            };
            push(ImGuiCol_WindowBg, ImVec4(0.055f, 0.075f, 0.145f, 0.955f));
            push(ImGuiCol_Border, ImVec4(0.91f, 0.66f, 0.14f, 0.55f));
            push(ImGuiCol_TitleBg, ImVec4(0.040f, 0.055f, 0.110f, 1.0f));
            push(ImGuiCol_TitleBgActive, ImVec4(0.11f, 0.14f, 0.30f, 1.0f));
            push(ImGuiCol_Text, ImVec4(0.93f, 0.90f, 0.82f, 1.0f));
            push(ImGuiCol_TextDisabled, ImVec4(0.58f, 0.60f, 0.70f, 1.0f));
            push(ImGuiCol_FrameBg, ImVec4(0.09f, 0.11f, 0.22f, 1.0f));
            push(ImGuiCol_FrameBgHovered, ImVec4(0.16f, 0.19f, 0.36f, 1.0f));
            push(ImGuiCol_FrameBgActive, ImVec4(0.22f, 0.26f, 0.46f, 1.0f));
            push(ImGuiCol_Button, ImVec4(0.11f, 0.14f, 0.28f, 1.0f));
            push(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.24f, 0.44f, 1.0f));
            push(ImGuiCol_ButtonActive, ImVec4(0.28f, 0.33f, 0.56f, 1.0f));
            push(ImGuiCol_SliderGrab, ImVec4(0.89f, 0.66f, 0.16f, 1.0f));
            push(ImGuiCol_SliderGrabActive, ImVec4(1.00f, 0.78f, 0.28f, 1.0f));
            push(ImGuiCol_CheckMark, ImVec4(0.91f, 0.66f, 0.14f, 1.0f));
            push(ImGuiCol_Separator, ImVec4(0.45f, 0.42f, 0.30f, 0.60f));
            push(ImGuiCol_Header, ImVec4(0.16f, 0.19f, 0.36f, 1.0f));
            PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
            PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
            PushStyleVar(ImGuiStyleVar_GrabRounding, 3.0f);
            pushedVars += 3; // style vars, popped on their own stack below
          }

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

            Separator();
            Text("Style");
            Checkbox("Van Gogh##style", &sb.vanGogh_);
            if (sb.vanGogh_) {
              SliderFloat("Impasto##impasto", &sb.impasto_, 0.4f, 2.0f);
              TextDisabled("  how thickly the paint is laid on");
              TextDisabled(
                  "  on: fields of brush strokes, a night sky with eleven stars,");
              TextDisabled("  glare on the fire and the water, and the weave of the");
              TextDisabled("  canvas under all of it.");
            } else {
              TextDisabled("  the automaton as a diagram: banded cells, one colour");
              TextDisabled("  per material, lit from above");
            }
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

          // Pop the studio's style now that both windows are closed -- after the
          // last End(), so the About dialog is styled too. Style colours and
          // style vars sit on two separate ImGui stacks, so each is popped with
          // its own count; an imbalance here raises a 'Missing PopStyle*' assert
          // every single frame.
          if (styled) {
            PopStyleColor(pushedColors);
            PopStyleVar(pushedVars);
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
