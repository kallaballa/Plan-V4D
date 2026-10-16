// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// How a material is drawn.
//
// This is the part with no counterpart upstream: SDL had exactly one way to put
// a colour on the screen, NanoVG has several. Two renderers, one function: the
// painter's path (Van Gogh mode, for cell sizes with room for a stroke) and the
// band path (the original diagram mode, and where small windows fall back to).
//
// Only resting types are ever asked about: settledType() has already folded
// each moved flavour back into its resting one by the time a bucket is drawn.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_RENDER_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_RENDER_HPP_

#include <algorithm>
#include <cmath>
#include <vector>

#include <opencv2/v4d/v4d.hpp>

#include "sandcraft_grid.hpp"
#include "sandcraft_materials.hpp"
#include "sandcraft_vangogh.hpp"
#include "sandcraft_view.hpp"

namespace sandcraft {

// How a material is drawn. This is the part with no counterpart upstream: SDL
// had exactly one way to put a colour on the screen, NanoVG has several.
//
// Only resting types are ever asked about: settledType() has already folded
// each moved flavour back into its resting one by the time a bucket is drawn.
enum class Material { SOLID, LIQUID, VAPOUR, EMBERISH, DISSOLVE };

inline Material materialOf(ParticleType t) {
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

// (litFace() and shadeFace(), the two colour helpers used here, sit next to
// the palette in sandcraft_materials.hpp: the panel's swatches darken and
// lighten with the same two functions.)

// The brush a material is laid on with, in Van Gogh's hand: how long a stroke
// runs, how wide it is loaded, how tightly it follows the field, and how much
// it leans out of the horizontal. Solids are combed into the field; liquids
// are laid in long horizontal dashes; fire and steam are steep and hot; acid
// is a stain rather than a body.
//
// The last three fields are the foreground treatment: the halo, the lit crown
// and the specular catches. They are what make a material read against a night
// painted from the same pigments -- the night sky is cobalt, and so are water,
// ice and the void, so without a light of their own those piles disappear.
inline vg::Brush brushOf(ParticleType type) {
  switch (materialOf(type)) {
  case Material::EMBERISH:
    return {3.6f, 0.90f, 255.0f, 0.6f, 1.25f, 250.0f, 64.0f, 100.0f, 0.0f};
  case Material::VAPOUR:
    return {3.6f, 0.95f, 165.0f, 0.8f, 1.10f, 90.0f, 38.0f, 76.0f, 0.0f};
  case Material::LIQUID:
    return {8.0f, 1.10f, 232.0f, 1.0f, 0.0f, 244.0f, 28.0f, 190.0f, 0.50f};
  case Material::DISSOLVE:
    return {5.0f, 0.95f, 232.0f, 0.7f, 0.0f, 150.0f, 30.0f, 140.0f, 0.45f};
  default:
    // A solid is laid with a loaded brush: strokes short enough to read as chop
    // rather than as long smears, wide enough to cover their row on their own,
    // and translucent enough that where two cross they mix.
    return {3.2f, 1.10f, 242.0f, 0.9f, 0.0f, 248.0f, 30.0f, 150.0f, 0.42f};
  }
}

// Fire's own glow, plus the reaction that lit it: a bolt for electricity, a
// sizzle for acid, a cloud for steam, a cold radiance for ice. Kept next to the
// brush so the two halves of a material's look are read together.
inline void reactionFx(const vg::Pigment &pig, ParticleType type,
                       const std::vector<Run> &runs, const View &v,
                       float timeSec) {
  switch (type) {
  case ELEC:
    vg::elecFx(pig, runs, v, timeSec);
    break;
  case TORCH:
    vg::fireFx(pig, runs, v, timeSec, 2.6f, true);
    break;
  case FIRE:
    vg::fireFx(pig, runs, v, timeSec, 1.7f, true);
    break;
  case EMBER:
    vg::emberFx(pig, runs, v, timeSec);
    break;
  case ACID:
    vg::acidFx(pig, runs, v, timeSec);
    break;
  case STEAM:
    vg::cloudFx(pig, runs, v, timeSec);
    break;
  case ICE:
    vg::frostFx(pig, runs, v, timeSec);
    break;
  default:
    break;
  }
}

inline void drawMaterial(const Palette &pal, ParticleType type,
                         const std::vector<Run> &runs, const View &v,
                         const Sandbox &sb, float timeSec, bool vanGogh,
                         float impasto) {
  using namespace cv::v4d::nvg;
  if (runs.empty())
    return;
  const cv::Scalar base = pal[type];
  const float cell = v.cell_;

  // --- the painter's path ---------------------------------------------------
  //
  // The strokes are sized in cells, not pixels, so a smaller cell simply means a
  // finer brush. The threshold is where a stroke stops being a stroke and starts
  // being a sub-pixel smear; above it every material in Van Gogh mode is painted
  // with the brush, and only a genuinely tiny window falls through to the band
  // renderer below.
  if (vanGogh && cell >= 1.7f) {
    static thread_local vg::Batch batch;
    const vg::Pigment pig = vg::pigmentOf(type);
    const vg::Brush brush = brushOf(type);

    vg::paintMaterial(pig, type, runs, v, sb, timeSec, brush, impasto, batch);

    // The broad glow the old renderer only gave to embers: every material now
    // carries a little of it, and the emissive ones carry a lot.
    switch (materialOf(type)) {
    case Material::EMBERISH:
      vg::glareRun(vg::mix(pig.light, vg::cream(), 0.30f), runs, v, cell * 1.9f,
                   88.0f, timeSec);
      break;
    default:
      break;
    }
    reactionFx(pig, type, runs, v, timeSec);
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

} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_RENDER_HPP_
