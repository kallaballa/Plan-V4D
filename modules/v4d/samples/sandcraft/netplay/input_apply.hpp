// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The authority's input application (specification.md §9.6, §11 step 2-4).
//
// This is the one place that turns an `InputEventWire` into a change on the
// authoritative state. It is deliberately shared: the M2 loopback self-test
// drives it directly (selftest.hpp) and the host simulation node drives it with
// the events the input node pushed, so the two can never disagree about what an
// event means (spec Rule S2 -- local and remote painting go through one path).
//
// Every value that arrives from a peer is hostile until proven otherwise, so
// each field is bounded before it is used: coordinates through Sandbox::inside,
// the brush through PARTICLE_TYPE_COUNT, the pen through [1,32], the emitter
// index through [0,4), and the command id through the known set (spec §9.6). A
// malformed stream therefore cannot crash the host; at worst it is ignored.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_INPUT_APPLY_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_INPUT_APPLY_HPP_

#include <algorithm>
#include <cstdint>

#include "../sandcraft_automaton.hpp"
#include "protocol.hpp"

namespace sandcraft {
namespace netplay {

// Apply one validated input event to `sb`'s state for `playerId`. Returns false
// if the event was ignored (out of range / unknown), true otherwise. The body is
// the old SandInputPlan::apply, with the single global brush/pen/paintCell
// replaced by the per-player PlayerState the event names.
inline bool applyInputEvent(Sandbox &sb, int playerId, const InputEventWire &e) {
  if (playerId < 0 || playerId >= 4)
    return false;
  PlayerState &p = sb.players_[playerId];
  const int x = e.x, y = e.y;
  switch (e.kind) {
  case EV_MOVE:
    p.cursorCell = cv::Point2d(double(x), double(y));
    return true;
  case EV_PRESS:
    if (sb.inside(x, y)) {
      if (e.brush < PARTICLE_TYPE_COUNT)
        p.brush = ParticleType(e.brush);
      p.penSize = std::clamp<int>(e.penSize, 1, 32);
      strokeBegin(sb, p, x, y);
    }
    return true;
  case EV_DRAG:
    if (sb.inside(x, y)) {
      if (e.brush < PARTICLE_TYPE_COUNT)
        p.brush = ParticleType(e.brush);
      p.penSize = std::clamp<int>(e.penSize, 1, 32);
      strokeExtend(sb, p, x, y);
    }
    return true;
  case EV_RELEASE:
    strokeEnd(p);
    return true;
  case EV_SET_BRUSH:
    if (e.brush < PARTICLE_TYPE_COUNT)
      p.brush = ParticleType(e.brush);
    return true;
  case EV_SET_PEN:
    p.penSize = std::clamp<int>(e.penSize, 1, 32);
    return true;
  case EV_COMMAND:
    switch (e.flags) {
    case CMD_PAUSE_TOGGLE:
      sb.world_.paused = !sb.world_.paused;
      return true;
    case CMD_CLEAR:
      sb.world_.clearRequested = true;
      return true;
    case CMD_RANDOMIZE:
      sb.world_.randomizeRequested = true;
      return true;
    case CMD_SWAPS_TOGGLE:
      sb.world_.particleSwaps = !sb.world_.particleSwaps;
      return true;
    case CMD_EMITTER_TOGGLE:
      if (e.brush < 4)
        sb.world_.emitters[e.brush].on = !sb.world_.emitters[e.brush].on;
      return true;
    case CMD_EMITTER_DENSITY:
      if (e.brush < 4)
        sb.world_.emitters[e.brush].density =
            std::clamp(sb.world_.emitters[e.brush].density +
                           float(e.dx) / 1000.0f,
                       0.05f, 1.0f);
      return true;
    default:
      return false;
    }
  default:
    return false;
  }
}

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_INPUT_APPLY_HPP_
