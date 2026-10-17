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
//
// The netplay design (see ../netplay/specification.md §7) splits the old flat
// fields by *ownership*: per-player state, host-authoritative world state and
// purely local state. Keeping the split here, rather than as loose members on
// the Sandbox, is what lets the simulation step later apply one player's brush
// without reaching for the host's -- and it is the seam the network code keys
// on. player 0 is always the local player, so the single-player demo reads and
// writes exactly the same cells it always did.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_SANDBOX_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_SANDBOX_HPP_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "sandcraft_materials.hpp"

namespace sandcraft {

// One human participant's brush and cursor. In the single-player demo there is
// exactly one, players_[0], and it is the one the panel and the input node
// drive. When the demo is networked the host owns all four and the clients'
// events are written into them (spec §11); colorIdx is the wire byte the HUD
// tints a peer's cursor ring with (spec §7.2, §16.5).
struct PlayerState {
  bool active = false;
  ParticleType brush = SAND;             // this player's selected material
  int penSize = 2;                       // 1..32
  cv::Point2d paintCell{-1.0, -1.0};     // host: stroke cursor for THIS player
  cv::Point2d cursorCell{-1.0, -1.0};    // hover cell, from MOVE events
  uint32_t appliedSeq = 0;               // host: highest seq folded into publish
  uint8_t colorIdx = 0;                  // 0..3 display colour
};

// Host-authoritative, simulation-affecting state. Every instance reads it, but
// only the authority writes it (spec Rule A3 / §9.5): a client's keystroke or
// panel click becomes a COMMAND the host validates and applies. epoch is bumped
// on clear/randomize so a replica can tell a world reset from a delta it merely
// missed (spec §18.2).
struct WorldState {
  bool paused = false;
  bool particleSwaps = true;
  std::array<Emitter, 4> emitters = {{
      {WATER, "Water", true, 0.30f},
      {SAND, "Sand", true, 0.30f},
      {SALT, "Salt", true, 0.30f},
      {OIL, "Oil", true, 0.30f},
  }};
  // One-shot commands the GUI raises and the simulation consumes, so that the
  // ImGui node never has to touch the grid itself. clear/randomize are world
  // effects and therefore live here, not on the local side (spec Rule S1).
  bool clearRequested = false;
  bool randomizeRequested = false;
  uint32_t epoch = 0; // ++ on clear/randomize (diagnostics)
};

// Purely local, never networked (spec §6, §7.2): the style, the panel/help
// windows and the HUD counters are each instance's own business. A client's
// Van Gogh toggle must not travel to the host, and neither must its fps.
//
// Van Gogh is off by default: the demo opens on the plain diagram (banded
// cells, one colour per material) and the oil-paint post pass only engages once
// the Style checkbox is ticked. The Weave/pigment path is the expensive one, so
// starting without it also keeps the first frames cheap on modest GPUs.
struct LocalState {
  bool vanGogh = false;
  float impasto = 1.0f;
  bool panelOpen = true;
  bool helpOpen = false;
  double fps = 0.0;
  double lastFrameTime = 0.0;
};

// The startup dialog's state (see sandcraft_menu.hpp). It lives on the shared
// sandbox so it survives across frames, and because the choice the player makes
// has to reach the worker that owns the graph branch. Only the display thread
// reads or writes it, inside the imgui() transaction, so the shared mutex is
// the only synchronisation it needs.
struct MenuState {
  bool open = true;             // the modal is up until a choice is committed
  bool openPopupRequested = true; // OpenPopup() must precede the first Begin
  int page = 0;                 // 0 = single/multiplayer, 1 = host/join
  bool focusHost = false;       // focus the host field when page 1 appears
  char host[256] = "127.0.0.1"; // hostname[:port] a client dials
  std::string error;            // set when the net runtime refuses to start
};

// A client-local, not-yet-acknowledged painted segment (spec §10). Owned by the
// client's plan nodes, flushed against the host's per-player watermark and never
// touched by the net thread. from == to for the first stamp of a stroke.
struct PredictedSegment {
  uint32_t seq = 0;     // input seq that produced it
  cv::Point2d from{-1.0, -1.0}, to{-1.0, -1.0};
  ParticleType brush = SAND;
  int penSize = 2;
};

struct Sandbox {
  // The virtual screen: one material per cell, row-major, width_ * height_.
  std::vector<ParticleType> cells_;
  int width_ = 0;
  int height_ = 0;

  // --- the participants, the world and this instance's own preferences ---
  std::array<PlayerState, 4> players_;
  WorldState world_;
  LocalState local_;
  // The startup dialog (sandcraft_menu.hpp) and the role the graph branch keys
  // on. hostMode_ used to be a plain member of SandcraftPlan, but the display
  // thread (which draws the dialog) and the compute worker (which evaluates the
  // branch) are *different plan instances*, so a per-instance member could not
  // carry the choice from one to the other. On the process-wide sandbox it is
  // one object with one mutex -- the dialog writes it under RWS(sandbox_) and
  // the branch reads it under RS(sandbox_.hostMode_) -- so a commit is visible
  // to the worker on the next frame (spec §16.4).
  MenuState menu_;
  bool hostMode_ = true;
  // Client-only optimistic strokes (spec §10); always empty on the authority.
  std::vector<PredictedSegment> predicted_;

  // Written by the simulation, read by the HUD. Derived, recomputed locally,
  // never transmitted (spec §7.5).
  int particleCount_ = 0;
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

  // The RNG. Upstream's utils::rand() is a two-word rotate/cross-add generator
  // (Utils.cpp), *not* the standard library's: every probability in the
  // automaton is written as `rand() % n` or `rand() >= RAND_MAX / 13` against
  // its full 32-bit output, so only the same generator reproduces the same
  // stream. It is carried over verbatim below -- same algorithm, same unseeded
  // initial words, same default time(0) seed -- so a fixed seed makes the port
  // and the original produce identical sequences. A member keeps its state next
  // to its user and single-threaded.
  uint32_t s_rand_high_ = 1u;
  uint32_t s_rand_low_ = 1u ^ 0x49616E42u;

  void srand(uint32_t seed) {
    s_rand_high_ = seed;
    s_rand_low_ = seed ^ 0x49616E42u;
  }
  void srand() { srand(uint32_t(std::time(nullptr))); }

  // utils::rand(): rotate both words by sizeof(int)/2, cross-add, advance the
  // low word. Unsigned overflow wraps, exactly as in the original.
  uint32_t rand32() {
    const int shift = int(sizeof(int) / 2);
    s_rand_high_ = (s_rand_high_ >> shift) + (s_rand_high_ << shift);
    s_rand_high_ += s_rand_low_;
    s_rand_low_ += s_rand_high_;
    return s_rand_high_;
  }

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

  // utils::rand() % n, with the zero guard the original lacked (no rule ever
  // passes 0, but a stray call must not trap).
  unsigned rand(unsigned n) { return n == 0 ? 0u : unsigned(rand32() % n); }

  void allocate(int w, int h) {
    width_ = w;
    height_ = h;
    cells_.assign(size_t(w) * size_t(h), NOTHING);
  }

  void clear() { std::fill(cells_.begin(), cells_.end(), NOTHING); }
};

} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_SANDBOX_HPP_
