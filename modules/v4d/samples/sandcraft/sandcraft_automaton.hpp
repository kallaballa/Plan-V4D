// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The automaton -- Particles.cpp, transcribed.
//
// Two halves, both plain functions over the Sandbox's cell grid, with no
// notion of a frame loop or a graphics context anywhere in them:
//
//   * painting into the grid -- the emitters, the brush disc and the brush
//     stroke, which is what the pointer and the top-of-screen emitters do; and
//   * one automaton step -- every cell of the play area, scanline by scanline.
//
// The transcription is deliberate, including the naming (MOVED-prefixed types,
// "virtual screen", the inert and floating bounds), so the two can be diffed
// against each other. Where the transcription needed to depart from a literal
// reading -- the original's `rand() >= RAND_MAX / 13` is a twelve-in-thirteen
// gate and not the one-in-thirteen it looks like, and its odd-inert types are
// exempt from the moved/unmoved settle -- the departure is called out at the
// site with the reason, because both are silent when you get them wrong: one
// makes the automaton an order of magnitude too slow to settle, the other
// deletes half the materials.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_AUTOMATON_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_AUTOMATON_HPP_

#include <algorithm>
#include <cmath>

#include "sandcraft_materials.hpp"
#include "sandcraft_sandbox.hpp"

namespace sandcraft {

// ---------------------------------------------------------------------------
// Drawing into the grid -- Particles.cpp
// ---------------------------------------------------------------------------

// Emitting a material at (x, 1) `width` cells wide, each cell with probability
// `p`.
inline void emit(Sandbox &sb, int x, int width, ParticleType type, float p) {
  for (int i = x - width / 2; i < x + width / 2; ++i) {
    if (float(sb.rand(1u << 20)) / float(1u << 20) < p)
      sb.put(i, 1, type);
  }
}

// A filled disc of `radius` cells, clipped to the grid.
inline void drawParticles(Sandbox &sb, int xpos, int ypos, int radius,
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
inline void drawLine(Sandbox &sb, int newx, int newy, int oldx, int oldy,
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
inline void doRandomLines(Sandbox &sb, ParticleType type, int penSize) {
  for (int i = 0; i < 20; ++i)
    drawLine(sb, int(sb.rand(unsigned(sb.width_))), 0,
             int(sb.rand(unsigned(sb.width_))), sb.height_ - 1, type, penSize);
  for (int i = 0; i < 20; ++i)
    drawLine(sb, 0, int(sb.rand(unsigned(sb.height_))), sb.width_ - 1,
             int(sb.rand(unsigned(sb.height_))), type, penSize);
}

// ---------------------------------------------------------------------------
// Per-player stroke helpers (spec §11, M0)
// ---------------------------------------------------------------------------
//
// The three motions of a brush, each taking the *player* whose brush and pen
// they use rather than reading the host's. In the single-player demo the player
// passed in is always players_[0]; the netplay input application calls the same
// functions with the sender's PlayerState, so remote and local painting cannot
// diverge (spec Rule S2). The logic is byte-for-byte the old SandInputPlan body.

// Begin a stroke: stamp a disc at the first cell.
inline void strokeBegin(Sandbox &sb, PlayerState &p, int x, int y) {
  drawLine(sb, x, y, x, y, p.brush, p.penSize);
  p.paintCell = cv::Point2d(double(x), double(y));
}

// Extend the stroke from where it currently is to (x, y), then move with it.
// A stroke with no origin (paintCell at (-1, -1)) stamps rather than rules a
// line across the world (spec §11 step 2).
inline void strokeExtend(Sandbox &sb, PlayerState &p, int x, int y) {
  const bool cont = p.paintCell.x >= 0.0 && p.paintCell.y >= 0.0;
  drawLine(sb, x, y, cont ? int(p.paintCell.x) : x,
           cont ? int(p.paintCell.y) : y, p.brush, p.penSize);
  p.paintCell = cv::Point2d(double(x), double(y));
}

// End a stroke: forget where it was, so the next press starts a fresh one.
inline void strokeEnd(PlayerState &p) { p.paintCell = {-1.0, -1.0}; }

// ---------------------------------------------------------------------------
// The rules -- Particles.cpp
// ---------------------------------------------------------------------------

// What an inert cell does. Note that the spouts (10..13) fall inside the inert
// range and are handled here, not in moveParticle(): they are fixed cells that
// shed a moving particle downwards.
inline void inertParticleLogic(Sandbox &sb, int x, int y, ParticleType type) {
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
inline void moveParticle(Sandbox &sb, int x, int y, ParticleType type) {
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
  if (sb.world_.particleSwaps) {
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

inline void updateVirtualPixel(Sandbox &sb, int x, int y) {
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
inline void updateVirtualScreen(Sandbox &sb) {
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

} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_AUTOMATON_HPP_
