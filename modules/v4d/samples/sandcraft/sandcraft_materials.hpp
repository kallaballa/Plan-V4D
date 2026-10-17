// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The material set -- Particles.hpp, verbatim.
//
// Everything the automaton, the renderer and the panel need to *name* a
// material: the type enum with its moved flavours, the bounds that partition
// the enum, the palette that gives each resting type one colour, and the brush
// table that maps the nineteen brushable materials onto names. The Emitter
// struct -- upstream GameState.hpp's emit*/dens* flags -- lives here too,
// because it is material data: a type, a name and how densely it is shed.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_MATERIALS_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_MATERIALS_HPP_

#include <algorithm>
#include <array>
#include <cstdint>
#include <opencv2/core.hpp>

namespace sandcraft {

// ---------------------------------------------------------------------------
// Enumerating conventions, carried over unchanged:
//   inert:    [INERT_LOWER_BOUND,    INERT_UPPER_BOUND]
//   floating: [FLOATING_LOWER_BOUND, FLOATING_UPPER_BOUND]
// ---------------------------------------------------------------------------
//
// Odd values are the "moved" flavour of the even type below them: the renderer
// draws `t - 1` for an odd `t` and settles the cell back to `t - 1`, which is
// what makes a material blink between resting and moving.
inline constexpr int INERT_LOWER_BOUND = 0;
inline constexpr int INERT_UPPER_BOUND = 13;
inline constexpr int FLOATING_LOWER_BOUND = 30;
inline constexpr int FLOATING_UPPER_BOUND = 33;

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

inline bool isInert(ParticleType t) {
  return t >= INERT_LOWER_BOUND && t <= INERT_UPPER_BOUND;
}
inline bool isFloating(ParticleType t) {
  return t >= FLOATING_LOWER_BOUND && t <= FLOATING_UPPER_BOUND;
}
// Burnable materials: plants and oils.
inline bool isBurnable(ParticleType t) {
  return t == PLANT || t == OIL || t == MOVEDOIL;
}
// ... of which only the plant smoulders rather than going up in flames.
inline bool burnsAsEmber(ParticleType t) { return t == PLANT; }

// ---------------------------------------------------------------------------
// The palette -- Palette.hpp, re-tinted for the Van Gogh look
// ---------------------------------------------------------------------------
//
// NanoVG's fill and stroke take BGRA, which is exactly cv::Scalar's channel
// order, so SDL_MapRGB(r,g,b) becomes cv::Scalar(b,g,r,255). The triples are
// *not* the original's, though: they are a warm, pigment-like retint chosen to
// sit under the oil-paint post pass, so the port does not render the original's
// exact colours. (Palette.hpp is the source of truth if that matters.)
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

// A brighter, warmer version of a colour -- the lit top face of a grain. Used
// by the renderer for the rim light on a heap of grains and by the panel for
// its hovered and active swatch states.
inline cv::Scalar litFace(cv::Scalar c, float amount) {
  const auto lift = [amount](double v) {
    return std::min(255.0, v + (255.0 - v) * amount);
  };
  return cv::Scalar(lift(c[0]), lift(c[1]), lift(c[2]), c[3]);
}
inline cv::Scalar shadeFace(cv::Scalar c, float amount) {
  const auto dim = [amount](double v) { return v * (1.0 - amount); };
  return cv::Scalar(dim(c[0]), dim(c[1]), dim(c[2]), c[3]);
}

// ---------------------------------------------------------------------------
// The brush palette -- Buttons.hpp, as data
// ---------------------------------------------------------------------------
//
// Upstream this was 19 AG_Buttons that a callback indexed into. Here it is the
// same 19 entries in the same order, consumed by the ImGui palette grid and by
// the LEFT/RIGHT brush-cycling keys.
struct BrushInfo {
  const char *name;
  ParticleType type;
};

inline constexpr std::array<BrushInfo, 19> kBrushes = {{
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

inline const char *brushName(ParticleType t) {
  for (const BrushInfo &b : kBrushes)
    if (b.type == t)
      return b.name;
  return "?";
}

// ---------------------------------------------------------------------------
// The emitters -- GameState.hpp's emit*/dens* flags, gathered into one struct
// ---------------------------------------------------------------------------
struct Emitter {
  ParticleType type;
  const char *name;
  bool on = true;
  float density = 0.3f;
};

} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_MATERIALS_HPP_
