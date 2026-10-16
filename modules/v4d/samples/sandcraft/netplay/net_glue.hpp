// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The one place the OpenCV-free netplay headers meet the OpenCV sample types.
//
// Everything under netplay/ except this file is deliberately free of the
// Sandbox: the codec, the protocol, the transports and the replication engine
// can be compiled and tested without OpenCV, which is what "prove each layer in
// isolation" (roadmap guiding principle 2) buys. This header is the seam where
// the two worlds are joined -- Sandbox <-> the wire structs -- and it is the
// only netplay header that includes the sample's state.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NET_GLUE_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NET_GLUE_HPP_

#include <cstdint>
#include <vector>

#include "../sandcraft_sandbox.hpp"
#include "grid_codec.hpp"
#include "netqueues.hpp"
#include "protocol.hpp"

namespace sandcraft {
namespace netplay {

static_assert(sizeof(ParticleType) == 1,
              "the wire grid is one byte per cell (spec §7.1)");

// --- world state (spec §7.2, §13.4) ------------------------------------------

inline WorldWire worldToWire(const WorldState &w) {
  WorldWire out{};
  out.paused = w.paused ? 1 : 0;
  out.particleSwaps = w.particleSwaps ? 1 : 0;
  out.epoch = w.epoch;
  for (int i = 0; i < 4; ++i) {
    out.emitters[i].type = uint8_t(w.emitters[i].type);
    out.emitters[i].on = w.emitters[i].on ? 1 : 0;
    out.emitters[i].density = w.emitters[i].density;
  }
  return out;
}

// Apply an authoritative WorldWire to a local WorldState. The local emitter
// *names* are not on the wire (they are a UI/locale concern), so they are left
// as they are; only type/on/density travel.
inline void applyWorldWire(const WorldWire &w, WorldState &out) {
  out.paused = w.paused != 0;
  out.particleSwaps = w.particleSwaps != 0;
  out.epoch = w.epoch;
  for (int i = 0; i < 4; ++i) {
    out.emitters[i].type = ParticleType(w.emitters[i].type);
    out.emitters[i].on = w.emitters[i].on != 0;
    out.emitters[i].density = w.emitters[i].density;
  }
}

// --- grid --------------------------------------------------------------------

inline Grid gridToBytes(const Sandbox &sb) {
  Grid g(sb.cells_.size());
  for (size_t i = 0; i < g.size(); ++i)
    g[i] = uint8_t(sb.cells_[i]);
  return g;
}

// Copy a decoded grid into the sandbox, sizing it if needed. The caller has
// already checked the dimensions against WELCOME (spec §23).
inline void gridToSandbox(const Grid &g, Sandbox &sb) {
  if (sb.cells_.size() != g.size())
    sb.cells_.resize(g.size());
  for (size_t i = 0; i < g.size(); ++i)
    sb.cells_[i] = ParticleType(g[i]);
}

// --- cursors / roster --------------------------------------------------------

inline PlayerCursorWire cursorToWire(const PlayerState &p) {
  PlayerCursorWire c{};
  // A hover cell outside the world is (-1,-1); the wire carries int16 cells.
  c.x = int16_t(p.cursorCell.x);
  c.y = int16_t(p.cursorCell.y);
  c.brush = uint8_t(p.brush);
  c.colorIdx = p.colorIdx;
  return c;
}

inline std::vector<PlayerWire> rosterToWire(const Sandbox &sb) {
  std::vector<PlayerWire> out;
  for (int i = 0; i < 4; ++i) {
    if (!sb.players_[i].active)
      continue;
    PlayerWire p{};
    p.id = uint8_t(i);
    p.colorIdx = sb.players_[i].colorIdx;
    p.active = 1;
    out.push_back(p);
  }
  return out;
}

// --- snapshot builder (spec §16.3) -------------------------------------------

inline GridSnapshot makeSnapshot(const Sandbox &sb, uint64_t tick) {
  GridSnapshot s;
  s.tick = tick;
  s.width = uint16_t(sb.width_);
  s.height = uint16_t(sb.height_);
  for (int i = 0; i < 4; ++i) {
    s.appliedSeq[i] = sb.players_[i].appliedSeq;
    s.cursors[i] = cursorToWire(sb.players_[i]);
  }
  s.cells = gridToBytes(sb);
  s.world = worldToWire(sb.world_);
  s.roster = rosterToWire(sb);
  return s;
}

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NET_GLUE_HPP_
