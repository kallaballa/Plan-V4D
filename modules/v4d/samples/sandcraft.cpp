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
#include "sandcraft/netplay/input_apply.hpp"
#include "sandcraft/netplay/net_glue.hpp"
#include "sandcraft/netplay/net_runtime.hpp"
#include "sandcraft/netplay/net_thread.hpp"
#include "sandcraft/netplay/netplay.hpp"
#include "sandcraft/netplay/netqueues.hpp"
#include "sandcraft/netplay/protocol.hpp"
#include "sandcraft/netplay/replication.hpp"
#include "sandcraft/netplay/selftest.hpp"
#include "sandcraft/netplay/tcp_selftest.hpp"

using namespace cv;
using namespace cv::v4d;
using namespace cv::v4d::event;

namespace sandcraft {

// The sub-plans talk to the netplay subsystem directly, so its small vocabulary
// of wire names is pulled in here rather than qualified at every use site.
using netplay::CMD_CLEAR;
using netplay::CMD_EMITTER_DENSITY;
using netplay::CMD_EMITTER_TOGGLE;
using netplay::CMD_PAUSE_TOGGLE;
using netplay::CMD_RANDOMIZE;
using netplay::EV_COMMAND;
using netplay::EV_DRAG;
using netplay::EV_MOVE;
using netplay::EV_PRESS;
using netplay::EV_RELEASE;
using netplay::EV_SET_BRUSH;
using netplay::EV_SET_PEN;
using netplay::InputBatchMsg;
using netplay::InputEnvelope;
using netplay::InputEventWire;
using netplay::MSG_INPUT;

// ===========================================================================
// The sub-plans
// ===========================================================================

// Stage 1 -- read the pointer and the keyboard, and turn them into input
// *events* (spec §9) rather than writing the grid directly.
//
// The events are the demo's one pointer/keyboard model: the host pushes its own
// into the loopback queue (so its own painting takes the identical path a
// peer's does -- spec Rule S2), and a guest frames them into INPUT batches for
// the wire. The conversion pointer->cell happens here, with the sender's own
// fitView, because the authority cannot reconstruct a peer's view (spec §9.1);
// the shared cellAt keeps it identical to the old SandInputPlan.
class LocalInputPlan : public V4DPlan {
public:
  Sandbox &sb_;
  explicit LocalInputPlan(Sandbox &sb) : sb_(sb) { _shared(sb_); }

  void infer() override {
    plain([this](Sandbox &sb, const cv::Size &fbSz, const cv::Size &winSz,
                 const Mouse::List &press, const Mouse::List &drag,
                 const Mouse::List &scroll, const Keyboard::List &keys,
                 const Mouse::List &move) {
            apply(sb, fbSz, winSz, press, drag, scroll, keys, move);
          },
          RWS(sb_), size_, windowSize_, press_, drag_, scroll_, keys_, move_);
  }

  // The pointer and the view have to agree, so the view is recomputed here
  // from the same size_ the renderer uses.
  void apply(Sandbox &sb, const cv::Size &fbSz, const cv::Size &winSz,
             const Mouse::List &press, const Mouse::List &drag,
             const Mouse::List &scroll, const Keyboard::List &keys,
             const Mouse::List &move) {
    // The role is a property of the *world*, not of this plan instance: the
    // startup dialog runs on the display thread and commits onto the shared
    // sandbox, so it is read here (under the same lock RWS(sb_) took) rather
    // than bound as a constructor reference (spec §16.4).
    const bool hostMode = sb.hostMode_;
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

    // A frame-local shadow of the local player's brush/pen: several wheel
    // steps or key presses in one frame must accumulate exactly as the old
    // in-place mutations did (spec §9.5). Painting is emitted first, so it uses
    // the frame-start brush, exactly as before.
    PlayerState shadow = sb.players_[0];

    std::vector<InputEventWire> events;
    auto event = [&](uint8_t kind, int x, int y) {
      InputEventWire e{};
      e.kind = kind;
      e.x = int16_t(x);
      e.y = int16_t(y);
      e.brush = uint8_t(shadow.brush);
      e.penSize = uint8_t(std::clamp(shadow.penSize, 1, 32));
      e.seq = seq_++;
      return e;
    };
    auto setBrush = [&](ParticleType b) {
      shadow.brush = b;
      InputEventWire e = event(EV_SET_BRUSH, -1, -1);
      e.brush = uint8_t(b);
      events.push_back(e);
    };
    auto setPen = [&](int pen) {
      shadow.penSize = std::clamp(pen, 1, 32);
      InputEventWire e = event(EV_SET_PEN, -1, -1);
      e.penSize = uint8_t(shadow.penSize);
      events.push_back(e);
    };
    auto command = [&](uint8_t cmd, uint8_t idx, int16_t dx) {
      InputEventWire e = event(EV_COMMAND, -1, -1);
      e.flags = cmd;
      e.brush = idx;
      e.dx = dx;
      events.push_back(e);
    };
    // Slots::previous/next_particle() in event form. The original searches the
    // button table from the end and keeps the brush where it is when the
    // current one is not in the table at all; here an unknown brush starts the
    // cycle at the first entry, which is one more step from the same place.
    auto cycleBrush = [&](int delta) {
      int cur = 0;
      for (size_t i = 0; i < kBrushes.size(); ++i)
        if (kBrushes[i].type == shadow.brush)
          cur = int(i);
      cur = (cur + delta + int(kBrushes.size())) % int(kBrushes.size());
      setBrush(kBrushes[size_t(cur)].type);
    };

    // --- painting ---
    //
    // A stroke is the polyline through this frame's stroke positions: the
    // press that starts it and every drag that continues it. The first point is
    // a disc (EV_PRESS -> strokeBegin) and the rest are segments (EV_DRAG ->
    // strokeExtend), which is exactly what the authority used to do in place.
    // Nothing else paints: a MOVE only moves the cursor.
    bool stroked = false;
    if (!sb.guiWantsMouse_) {
      std::vector<cv::Point2d> stroke;
      stroke.reserve(press.size() + drag.size());
      const auto at = [](const cv::Point &p) {
        return cv::Point2d(double(p.x), double(p.y));
      };
      for (const auto &e : press)
        stroke.push_back(at(e->position()));
      for (const auto &e : drag)
        stroke.push_back(at(e->position()));
      stroked = !stroke.empty();
      bool head = true;
      for (const cv::Point2d &p : stroke) {
        const cv::Point2d c = cellAt(v, sb, p, winSz);
        if (c.x < 0.0)
          continue; // off the world; the authority's stroke ignores it too
        events.push_back(event(head ? EV_PRESS : EV_DRAG, int(c.x), int(c.y)));
        head = false;
      }
    }
    // The old node ended the stroke on every frame with no press/drag; the
    // authority does the same, but end it only when a stroke was open, so no
    // empty RELEASEs reach the wire.
    if (stroked)
      painting_ = true;
    else if (painting_) {
      events.push_back(event(EV_RELEASE, -1, -1));
      painting_ = false;
    }

    // --- the hover cell ---
    // The ring follows the pointer whether or not it is painting; the cell is
    // recomputed and sent every frame, so the authority's cursorCell tracks the
    // sender exactly as the old in-place assignment did.
    {
      const cv::Point2d c = sb.guiWantsMouse_ ? cv::Point2d(-1.0, -1.0)
                                              : cellAt(v, sb, windowPt, winSz);
      events.push_back(event(EV_MOVE, int(c.x), int(c.y)));
    }

    // --- keys: the keymap from the original help window ---
    if (!sb.guiWantsKeyboard_) {
      for (const auto &e : keys) {
        switch (e->key()) {
        case Keyboard::DELETE:
          command(CMD_CLEAR, 0, 0);
          break;
        case Keyboard::LEFT:
          cycleBrush(-1);
          break;
        case Keyboard::RIGHT:
          cycleBrush(+1);
          break;
        case Keyboard::DOWN:
          setPen(std::max(1, shadow.penSize / 2));
          break;
        case Keyboard::UP:
          setPen(std::min(32, shadow.penSize * 2));
          break;
        case Keyboard::ENTER:
          for (uint8_t i = 0; i < 4; ++i) command(CMD_EMITTER_TOGGLE, i, 0);
          break;
        case Keyboard::BACKSPACE:
          setBrush(NOTHING);
          break;
        case Keyboard::MINUS:
          for (uint8_t i = 0; i < 4; ++i) command(CMD_EMITTER_DENSITY, i, -50);
          break;
        // '+' and '=' are the same physical key on a US layout.
        case Keyboard::EQUAL:
        case Keyboard::KP_ADD:
          for (uint8_t i = 0; i < 4; ++i) command(CMD_EMITTER_DENSITY, i, +50);
          break;
        case Keyboard::R:
          command(CMD_RANDOMIZE, 0, 0);
          break;
        case Keyboard::SPACE:
          command(CMD_PAUSE_TOGGLE, 0, 0);
          break;
        // Slots::toggle_tools(): the original's TAB showed and hid the whole
        // dashboard. Here the dashboard is the panel below, so it is the panel
        // that toggles -- and it is the only way back once it is closed, since
        // the close button is on the panel itself. Local, never networked.
        case Keyboard::TAB:
          sb.local_.panelOpen = !sb.local_.panelOpen;
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
        setPen(std::clamp(int(shadow.penSize * (dy > 0 ? 2.0f : 0.5f)), 1, 32));
      } else if (sb.ctrlDown_) {
        for (uint8_t i = 0; i < 4; ++i)
          command(CMD_EMITTER_DENSITY, i, dy > 0 ? int16_t(50) : int16_t(-50));
      } else if (sb.altDown_) {
        // Slots::toggle_emit() on ALT+wheel, before the brush fallback.
        for (uint8_t i = 0; i < 4; ++i) command(CMD_EMITTER_TOGGLE, i, 0);
      } else {
        cycleBrush(dy > 0 ? -1 : +1);
      }
    }

    // --- dispatch ---
    if (hostMode) {
      // The authority's own input takes the loopback path so that it cannot
      // diverge from a peer's (spec §16.4, Rule S2); the simulation drains it
      // in this same tick.
      for (const InputEventWire &ev : events)
        netplay::netQueues().pushInput({uint8_t(0), ev});
    } else {
      // The guest sends every event to the authority (no rate limiting, no
      // coalescing -- spec §9.3) and mirrors its own cursor locally for the
      // ring. The replica itself belongs to the authority.
      if (!events.empty()) {
        netplay::InputBatchMsg batch;
        batch.b.batchTimeMs = netplay::nowMillis();
        batch.b.firstSeq = events.front().seq;
        batch.b.count = uint16_t(events.size());
        batch.events = events;
        netplay::netQueues().pushOutbound(netplay::frame(
            netplay::MSG_INPUT, ++msgSeq_, netplay::encodePayload(batch)));
      }
      for (const InputEventWire &ev : events)
        if (ev.kind == netplay::EV_MOVE)
          sb.players_[0].cursorCell = cv::Point2d(ev.x, ev.y);
    }
  }

private:
  Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);
  Property<cv::Size> windowSize_ = P<cv::Size>(V4D::Keys::WINDOW_SIZE);
  Event<Mouse> press_ = E<Mouse>(Mouse::PRESS, Mouse::LEFT);
  Event<Mouse> drag_ = E<Mouse>(Mouse::DRAG, Mouse::LEFT);
  Event<Mouse> scroll_ = E<Mouse>(Mouse::SCROLL);
  Event<Mouse> move_ = E<Mouse>(Mouse::MOVE);
  Event<Keyboard> keys_ = E<Keyboard>(Keyboard::PRESS);
  // Per-emitter monotonic input sequence (spec §9.4), the stroke state the old
  // in-place painter kept implicitly in paintCell, and the guest's own message
  // counter for the frames it sends.
  uint32_t seq_ = 0;
  bool painting_ = false;
  uint32_t msgSeq_ = 0;
};

// Stage 2 -- the automaton. One plain() node; the heavy work of the frame.
class SandSimulationPlan : public V4DPlan {
public:
  Sandbox &sb_;
  SandSimulationPlan(Sandbox &sb) : sb_(sb) { _shared(sb_); }

  void infer() override { plain(&SandSimulationPlan::step, RWS(sb_)); }

  static void step(Sandbox &sb) {
    // The authority's tick drains every input event queued since the last tick
    // and applies it in arrival order (spec §11 steps 1-4). Its own input
    // arrives through the same loopback queue a peer's does (Rule S2), so there
    // is exactly one apply path; the sender id travels with the event.
    {
      std::vector<netplay::InputEnvelope> input;
      netplay::netQueues().drainInput(input);
      for (const netplay::InputEnvelope &env : input)
        netplay::applyInputEvent(sb, env.player, env.event);
    }

    // The one-shot commands from the GUI, consumed here so that the ImGui node
    // never has to touch the grid. They live on the world state because they
    // are world effects (spec Rule S1); on a client this whole node is replaced
    // by the delta application (spec §16.4).
    if (sb.world_.clearRequested) {
      sb.clear();
      sb.world_.clearRequested = false;
      ++sb.world_.epoch;
    }
    if (sb.world_.randomizeRequested) {
      PlayerState &me = sb.players_[0];
      doRandomLines(sb, me.brush, me.penSize);
      sb.world_.randomizeRequested = false;
      ++sb.world_.epoch;
    }
    if (sb.world_.paused)
      return;

    for (size_t i = 0; i < sb.world_.emitters.size(); ++i) {
      const Emitter &e = sb.world_.emitters[i];
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
    // The authoritative state for this tick is the *settled* grid (spec §7.4,
    // Rule A4). Settling here, not during drawing, is what makes the published
    // state render-independent: a replica that never draws still converges.
    settleGrid(sb);
    ++sb.simFrames_;

    // Publish the settled grid for the network thread (spec §16.3). Only when a
    // session is live: the 230 KB snapshot copy is skipped entirely in
    // single-player, so the no-argument demo pays nothing for netplay.
    if (netplay::netActive())
      netplay::netQueues().publishSnapshot(std::make_shared<netplay::GridSnapshot>(
          netplay::makeSnapshot(sb, uint64_t(sb.simFrames_))));
  }
};

// Stage 2b (guest only) -- apply the authority's frames to the replica
// (spec §12, §16.4). The host branch of the graph never runs this; on a guest
// it replaces SandSimulationPlan, which is why a guest can never step the
// automaton on its own (spec §7.6, Rule A1). Until a session is live its
// inbound queue is empty, so a client boots on the seeded grid and simply waits
// for a keyframe.
class NetApplyPlan : public V4DPlan {
public:
  Sandbox &sb_;
  NetApplyPlan(Sandbox &sb) : sb_(sb) { _shared(sb_); }

  void infer() override {
    plain([this](Sandbox &sb) { apply(sb); }, RWS(sb_));
  }

  void apply(Sandbox &sb) {
    std::vector<std::vector<uint8_t>> replies;
    netplay::Frame f;
    while (netplay::netQueues().popInbound(f)) {
      if (f.oversize)
        continue;
      replies.clear();
      const netplay::ApplyResult r = client_.apply(f, &replies);
      // A checksum mismatch or a base gap means the replica cannot trust its
      // history: ask for a fresh keyframe and keep rendering the last good grid
      // (spec §12.3, §18.2).
      if (r == netplay::ApplyResult::Desync)
        replies.push_back(client_.resync());
      for (std::vector<uint8_t> &reply : replies)
        netplay::netQueues().pushOutbound(std::move(reply));
    }
    if (client_.haveState()) {
      netplay::gridToSandbox(client_.grid(), sb);
      netplay::applyWorldWire(client_.world(), sb.world_);
    }
  }

private:
  netplay::ClientReplicator client_;
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
    clearScreen(sb.local_.vanGogh ? cv::Scalar(24, 17, 9, 255)
                                  : cv::Scalar(10, 10, 14, 255));
    const View v = fitView(sz, sb.width_, sb.height_);

    // An exponential average over the frame interval. `seconds()` is the
    // runtime's clock, so this is the presented rate, not the sim rate. The
    // previous frame's timestamp lives in the sandbox rather than in a
    // function-local static: a static here would be one variable shared by
    // every worker rendering this plan, so with more than one worker each
    // would compute the interval against the other's frame.
    if (sb.local_.lastFrameTime > 0.0 && timeSec > sb.local_.lastFrameTime) {
      const double instant = 1.0 / (timeSec - sb.local_.lastFrameTime);
      sb.local_.fps = sb.local_.fps > 0.0 ? sb.local_.fps * 0.9 + instant * 0.1
                                          : instant;
    }
    sb.local_.lastFrameTime = timeSec;

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

  // Which side of the star this process is (spec §5, §16.4) now lives on the
  // shared sandbox as sandbox_.hostMode_, not on a plan member: the display
  // thread (which runs the startup dialog) and the compute worker (which
  // evaluates the branch) are two different plan instances, so only an object
  // with one process-wide address and one mutex can carry the choice between
  // them. The constructor seeds it from the NetRuntime (the command-line role);
  // the startup dialog may flip it afterwards, and the per-frame branch below
  // sees that on the next frame.

  cv::Ptr<LocalInputPlan> input_;
  cv::Ptr<SandSimulationPlan> sim_;
  cv::Ptr<NetApplyPlan> netApply_;
  cv::Ptr<SandRenderPlan> render_;
  // The oil-paint surface. It is owned per plan instance and touched only by
  // this plan's gl() nodes on this instance's GL context, so -- like CubeScene's
  // scene_ -- it is never shared and needs no mutex.
  VanGoghPost post_;

  Property<cv::Size> size_ = P<cv::Size>(V4D::Keys::SIZE);

public:
  SandcraftPlan() {
    _shared(sandbox_);

    // The role is a process-wide decision (NetRuntime, spec §16.6), made once
    // from argv before the first frame -- or, when no role was given, made
    // later by the startup dialog (sandcraft_menu.hpp), which commits onto the
    // same shared sandbox_. Seeding it here means an explicit --host/--connect
    // (or --single-player) is in force before the graph is recorded; the dialog
    // then flips sandbox_.hostMode_ at runtime and the branch follows.
    sandbox_.hostMode_ = netplay::netRuntime().host();

    // The runtime builds one plan instance per compute worker *and* one for the
    // display thread, so this constructor runs more than once per process (with
    // run<T>(0) -- a single compute worker -- exactly twice). Building the grid
    // here unconditionally would therefore re-run it per instance, which is
    // harmless only as long as both happen before the first frame; the guard
    // makes "the world is allocated and seeded exactly once" a property of the
    // code rather than a property of the runtime's construction order.
    //
    // 640x360, the original's resolution; nothing in the
    // automaton depends on it, and the renderer fits whatever it is given to
    // the viewport.
    if (sandbox_.cells_.empty()) {
      sandbox_.allocate(640, 360);
      seed();
    }
    // Player 0 is the local player and always present (spec §5); the flag is
    // what puts it in the roster a joining client receives.
    sandbox_.players_[0].active = true;

    input_ = _sub<LocalInputPlan>(this, sandbox_);
    sim_ = _sub<SandSimulationPlan>(this, sandbox_);
    netApply_ = _sub<NetApplyPlan>(this, sandbox_);
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
    if (sb.local_.vanGogh)
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
    // Input, then (host) simulate or (guest) apply, then draw: the graph is the
    // pipeline, and the order is the order the sub-plans are spliced in. The
    // whole painting is drawn with nvg() (render_), varnished and set glowing by
    // a gl() pass, and the crisp chrome is laid over it with a second nvg().
    //
    // The role split (spec §16.4): exactly one of SandSimulationPlan (the
    // authority steps the automaton) or NetApplyPlan (a guest applies the
    // authority's keyframes/deltas) runs. The two are mutually exclusive, so a
    // guest can never step the automaton (spec Rule A1, §7.6). The predicate is
    // re-evaluated every frame, so the startup dialog can commit host or client
    // after the graph was recorded and the worker takes the matching arm on the
    // next frame; on an explicit --host/--connect it never changes.
    subInfer(input_);
    branch(RS(sandbox_.hostMode_))
        ->subInfer(sim_)
    ->elseBranch()
        ->subInfer(netApply_)
    ->endBranch();
    subInfer(render_);                                          // nvg: the painting
    gl(&SandcraftPlan::postProcess, RWS(sandbox_), RW(post_));  // gl: varnish + glow
    nvg(&SandcraftPlan::overlayDraw, RWS(sandbox_), size_);      // nvg: crisp chrome
  }
};

Sandbox SandcraftPlan::sandbox_;

V4D_DEMO_MAIN(int argc, char **argv) {
  cv::v4d::add_asset_search_paths();

  // The netplay command line (spec §16.6); M0 parses and reports it, and later
  // milestones branch the graph on the role behind this same config. With no
  // arguments the role is Host and the sample behaves exactly as it always did
  // (spec §4.3). Args are null on Android, where the default is host.
  const netplay::NetConfig net = netplay::parseArgs(argc, argv);
  if (net.help) {
    std::cout << netplay::usageText();
    return 0;
  }

  // The in-process host==client self-test (roadmap M2, spec §15.3 rung 0). It
  // runs the whole replication and input pipeline over the loopback transport
  // before any window exists, so it is a headless run that ends in an exit code
  // -- which is exactly how the toolkit judges it. Nothing below this line runs
  // when it is asked for.
  if (net.selfTest) {
    const netplay::SelfTestStats st = netplay::runSelfTest(net, std::cerr);
    return st.failures == 0 ? 0 : 1;
  }

  // The in-process host+client self-test over real loopback TCP (roadmap M3).
  // Two net threads, two sockets, one automaton: the milestone's headless proof
  // that the framing, the handshake, replication and input survive a real byte
  // stream. Also exits non-zero on any failed check.
  if (net.tcpSelfTest) {
    const netplay::TcpSelfTestStats st = netplay::runTcpSelfTest(net, std::cerr);
    return st.failures == 0 ? 0 : 1;
  }

  // Apply the role before any plan instance exists (spec §16.6). SandcraftPlan
  // reads the singleton in its constructor to seed the graph branch, so the
  // configure() has to precede V4D::init(). With no role argument this is the
  // offline default (Host, no socket); the startup dialog then commits the
  // player's choice at runtime by reconfiguring the same singleton and flipping
  // the shared sandbox flag.
  netplay::netRuntime().configure(net);

  // Start the net thread only when the process was asked to network *up front*
  // (opt-in, spec §16.6). A no-argument run -- which opens the startup dialog --
  // opens no socket here; the dialog starts it if the player picks host or join.
  // A no-menu run opens nothing at all and skips the per-tick snapshot copy
  // entirely (§4.3).
  if (net.enabled) {
    std::string err;
    if (!netplay::netRuntime().start(err))
      std::cerr << "sandcraft: net start failed: " << err << std::endl;
  }

  std::cerr << "sandcraft: net role="
            << (net.role == netplay::Role::Host ? "host" : "client")
            << " proto=" << int(netplay::PROTOCOL_VERSION);
  if (net.role == netplay::Role::Client)
    std::cerr << " connect=" << netplay::endpointHost(net) << ":"
              << netplay::endpointPort(net);
  else if (net.enabled)
    std::cerr << " listening=" << netplay::netRuntime().boundPort();
  else if (net.menu)
    std::cerr << " (startup menu)";
  else
    std::cerr << " (single player)";
  std::cerr << " name=" << net.name << std::endl;

  // The Van Gogh brush path only engages above ~2.6 px/cell, so a 640x360 grid
  // needs a large window to keep the ~3.4 px/cell stroke scale the demo was
  // tuned at (2x the grid => 2x the old 1280x720 canvas). A smaller window just
  // falls through to the banded diagram; the renderer refits on resize, so a
  // user may still shrink it on a modest screen.
  cv::Rect viewport(0, 0, 1920, 1080);
  cv::Ptr<V4D> runtime = V4D::init(
      viewport, "Sandcraft", AllocateFlags::NANOVG | AllocateFlags::IMGUI, ConfigFlags::DEFAULT | ConfigFlags::DISPLAY_MODE);
  V4DPlan::run<SandcraftPlan>(0);
  netplay::netRuntime().stop();
  return 0;
}
