// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The scene: the world, the emitters, the in-world annotations, and the chrome
// that stays crisp.
//
// Everything is in framebuffer pixels derived from the fitted cell size, so it
// is correct at any window size.
//
// The world fills the window: there is no frame, no studio wall around the
// slab and no reserved band for the chrome, so the painting runs edge to edge.
// The chrome that used to live outside the world -- the emitter nozzles and the
// HUD -- is drawn as a translucent overlay inside it instead.
//
// Two styles, one function. The band path is the original: it is where small
// windows fall back to (below ~2.6 px a cell there is no room for a stroke),
// and where the panel's checkbox returns to. The painter's path stages the
// scene instead -- a night the world hangs in, the world as strokes over the
// stars, and the varnish last.
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

// The emitter nozzles and their names, painted as a translucent overlay onto
// the top of the world. They used to sit in a strip reserved above the slab;
// now that the world fills the window there is no such strip, so they hang
// from the top edge of the painting instead -- a soft scrim seats the names so
// a cream label reads over a starry sky or over a heap of sand alike.
//
// Drawn *after* the world (sky and grid), because unlike before it is now over
// the play area rather than beside it.
inline void drawEmittersOverlay(const Palette &pal, Sandbox &sb, const View &v,
                                double timeSec) {
  using namespace cv::v4d::nvg;
  const float cell = v.cell_;
  const float w = cell * v.width_;
  const cv::Point2f o = v.origin_;
  const bool vg = sb.local_.vanGogh;
  const float tsec = float(timeSec);
  const cv::Scalar cream = vg::cream();

  // The strip the nozzles and their names live in, and the wash that seats them
  // over whatever the world has piled up underneath.
  const float band = std::max(28.0f, cell * 7.0f);
  {
    const Paint seat = linearGradient(
        0.0f, o.y, 0.0f, o.y + band,
        cv::Scalar(0, 0, 0, vg ? 110.0f : 130.0f), cv::Scalar(0, 0, 0, 0));
    beginPath();
    rect(o.x, o.y, w, band);
    fillPaint(seat);
    fill();
  }

  fontFace("sans");
  const float nozzleFont = std::max(11.0f, std::min(cell * 1.4f, 20.0f));
  fontSize(nozzleFont);
  textAlign(NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
  for (size_t i = 0; i < sb.world_.emitters.size(); ++i) {
    const Emitter &e = sb.world_.emitters[i];
    const float cx = o.x + (emitterColumn(sb.width_, i) + 0.5f) * cell;
    // The mouth hangs just inside the top edge; the stem and the name sit above
    // it within the band, so the whole fixture stays on screen at any size.
    const float cy = o.y + band - 6.0f;
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
        fillPaint(radialGradient(cx, gy, 0.0f, cell * 1.0f, g,
                                 cv::Scalar(0, 0, 0, 0)));
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
}

inline void drawWorld(const Palette &pal, Sandbox &sb, const View &v,
                      double timeSec) {
  using namespace cv::v4d::nvg;
  const float cell = v.cell_;
  const float w = cell * v.width_;
  const float h = cell * v.height_;
  const cv::Point2f o = v.origin_;
  const bool vg = sb.local_.vanGogh;
  const float tsec = float(timeSec);

  // ---- the studio wall and the frame are gone ------------------------------
  //
  // The world used to hang on a linen studio wall inside a gilded frame, with
  // the wall showing around the slab. The world now *is* the window, so there is
  // no around: the linen would be fully overdrawn by the sky (and the frame
  // would sit on top of the painting), so neither is drawn. What decoration
  // remains is the gl() varnish, which vignettes the corners of the finished
  // canvas (sandcraft_post.hpp).
  // --- what the world is painted on, and over ------------------------------
  if (vg) {
    // Starry Night, as the empty cells: the painted night is laid down first
    // and the world is painted over it, so every pile hangs in a sky rather
    // than on a tray.
    scissor(o.x, o.y, w, h);
    vg::paintSky(o.x, o.y, w, h, tsec);
    // The scrim separates the world from the sky it hangs in, so the piles do
    // not dissolve into a night painted from the same pigments they are.
    vg::paintScrim(o.x, o.y, w, h);
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
    drawMaterial(pal, t, runs[t], v, sb, tsec, vg, sb.local_.impasto);

  if (vg) {
    // The linen weave is intentionally NOT laid over the paint here: on the
    // heaps it reads as a greyish criss-cross that fights the brushwork. The
    // paint is left clean.
    resetScissor();
  }

  // The emitter nozzles and their names, over the top of the painting.
  drawEmittersOverlay(pal, sb, v, timeSec);
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
  const bool vg = sb.local_.vanGogh;
  const cv::Scalar cream = vg::cream();

  // --- the brush ring ---
  const PlayerState &me = sb.players_[0];
  if (me.cursorCell.x >= 0.0 && cell > 0.75f) {
    const float r = (me.penSize + 0.5f) * cell;
    const float cx = o.x + (me.cursorCell.x + 0.5f) * cell;
    const float cy = o.y + (me.cursorCell.y + 0.5f) * cell;
    const cv::Scalar col = pal[me.brush];
    if (vg) {
      const vg::Pigment pig = vg::pigmentOf(me.brush);
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

  // --- the HUD, an overlay along the foot of the world ----------------------
  //
  // The world fills the window, so the HUD is painted onto its foot rather than
  // into a band under it: a scrim darkens the last rows of the painting and the
  // readouts sit on that, legible over sky or over a heap of sand.
  const float bandH = std::max(40.0f, 46.0f * chromeScale(sz));
  const float hudTop = o.y + h - bandH;
  {
    const Paint seat = linearGradient(
        0.0f, hudTop, 0.0f, o.y + h, cv::Scalar(0, 0, 0, 0),
        cv::Scalar(0, 0, 0, vg ? 120.0f : 140.0f));
    beginPath();
    rect(o.x, hudTop, w, bandH);
    fillPaint(seat);
    fill();
  }
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
  paintText(title.c_str(), title.c_str() + title.size(), o.x, hudTop + 6.0f,
            vg ? cream : cv::Scalar(225, 225, 240, 235));

  fontFace("sans");
  fontSize(12.5f);
  char stats[256];
  std::snprintf(stats, sizeof(stats),
                "%d particles   %.0f fps   %dx%d cells   %s, size %d",
                sb.particleCount_, sb.local_.fps, sb.width_, sb.height_,
                brushName(me.brush), me.penSize);
  paintText(stats, stats + std::strlen(stats), o.x, hudTop + 26.0f,
            vg ? vg::withAlpha(cream, 215.0f) : cv::Scalar(150, 150, 170, 230));

  textAlign(NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
  constexpr const char *kHint =
      "drag to paint   wheel: brush   SHIFT+wheel: pen   DELETE: clear";
  paintText(kHint, kHint + std::strlen(kHint), sz.width - o.x, hudTop + 26.0f,
            vg ? vg::withAlpha(cream, 165.0f) : cv::Scalar(120, 120, 140, 200));

  // The varnish -- the lit-from-the-room vignette -- is not chrome: the gl()
  // node lays it over the painting, under this overlay, so it darkens the paint
  // and not the readouts.
}

} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_SCENE_HPP_
