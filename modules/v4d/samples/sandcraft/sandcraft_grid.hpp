// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The world as drawable runs -- where fb() would have gone, nvg() does instead.
//
// A horizontal run of one material: the run-length merge that replaces the
// per-particle blit. A row of settled sand is one rectangle, not three hundred.
// This is the seam between the automaton and the renderer: the grid is walked
// once, every cell's moved flavour is settled back to its resting type, and the
// runs are bucketed per material for the drawing code to consume.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_GRID_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_GRID_HPP_

#include <array>
#include <vector>

#include "sandcraft_materials.hpp"
#include "sandcraft_sandbox.hpp"

namespace sandcraft {

struct Run {
  int x0_, x1_, y_; // inclusive x0_ and x1_
};
using RunBuckets = std::array<std::vector<Run>, PARTICLE_TYPE_COUNT>;

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
inline ParticleType settledType(ParticleType raw) {
  return (!isInert(raw) && (raw % 2) == 1) ? ParticleType(raw - 1) : raw;
}

inline int collectRuns(Sandbox &sb, RunBuckets &runs) {
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

} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_GRID_HPP_
