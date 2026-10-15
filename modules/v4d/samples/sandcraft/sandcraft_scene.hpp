// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The scene: the world slab, the emitters, the in-world annotations, and the
// chrome that stays crisp.
//
// Everything is in framebuffer pixels derived from the fitted cell size, so it
// is correct at any window size.
//
// Two styles, one function. The band path is the original: it is where small
// windows fall back to (below ~2.6 px a cell there is no room for a stroke),
// and where the panel's checkbox returns to. The painter's path stages the
// scene instead -- linen on the studio wall, a gilded frame, a night the world
// hangs in, the world as strokes over the stars, the canvas weave once more
// over the paint and the varnish last.
//
// drawChrome is the second half: the brush ring and the HUD, drawn on top of
// the finished painting -- after the gl() varnish pass -- so the cursor and
// the readouts stay legible over the paint rather than melting into it.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_SCENE_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_SCENE_HPP_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <opencv2/v4d/v4d.hpp>

#include "sandcraft_grid.hpp"
#include "sandcraft_materials.hpp"
#include "sandcraft_render.hpp"
#include "sandcraft_sandbox.hpp"
#include "sandcraft_vangogh.hpp"
#include "sandcraft_view.hpp"

namespace sandcraft {

inline void drawWorld(const Palette &pal, Sandbox &sb, const View &v,
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
                  cell * 0.7f, 24.0f + 20.0f * glow);
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
    // The linen weave is intentionally NOT laid over the paint here: on the
    // heaps it reads as a greyish criss-cross that fights the brushwork. A faint
    // canvas is still felt behind the slab (the studio-wall weave drawn at the
    // top of this function); the paint itself is left clean.
    resetScissor();
  }
}

// The chrome that stays crisp: the brush ring and the HUD. It is drawn on top
// of the finished painting -- after the gl() varnish pass -- so the cursor and
// the readouts stay legible over the paint rather than melting into it.
inline void drawChrome(const Palette &pal, Sandbox &sb, const View &v,
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
  const std::string title = "Sandcraft";
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

} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_SCENE_HPP_
