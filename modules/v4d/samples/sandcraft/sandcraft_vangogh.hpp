// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The Van Gogh treatment -- where the palette becomes paint.
//
// NanoVG has no single way of putting a colour on the screen: there are flat
// fills, gradients, tileable images, additive composites and arbitrary paths,
// and all of them are one call away. A flat fill per cell is what the original
// did (one SDL pixel per particle). What follows is what an oil painter would
// have done with the same primitives.
//
// Four ideas, all of them taken from the paintings rather than from a renderer:
//
//   * **A ground, then paint on top of it.** Every oil painting starts with a
//     flat "imprimatura" that covers the canvas and only then receives strokes.
//     Here the ground is one wavy rectangle per run of one material, so the
//     pile is opaque and cheap to draw, and the strokes above it are pure
//     texture. Without it, translucent strokes would let the night sky shine
//     straight through a heap of sand.
//   * **A stroke is a tapered, wobbly ribbon, not a capsule.** A bezier bulged
//     belly with a ragged edge reads as a loaded brush; a capsule reads as a
//     line. The geometry is two beziers out and two beziers back, with a
//     constant half-width and a perpendicular wobble on each edge.
//   * **Shadow, body, light, and one note of the complement.** Van Gogh almost
//     never laid a colour next to itself: blue against orange, yellow against
//     violet, with a thin stroke of the complementary hue as the last passage.
//     Four passes per material is the whole trick.
//   * **The brush flow is combed, and it is stable.** The strokes of a field
//     run in two alternating directions, which is why "Wheatfield with Crows"
//     reads as wind. The direction here is a hash of the cell deformed slowly
//     by time, so it never re-rolls from frame to frame -- a field of dashes
//     whose direction flickers reads as television static.
//
// Everything deterministic comes out of a private hash, never out of the
// Sandbox's RNG: that stream belongs to the automaton, and a renderer that
// consumed it would change the physics by looking at it.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_VANGOGH_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_VANGOGH_HPP_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/v4d/v4d.hpp>

#include "sandcraft_grid.hpp"
#include "sandcraft_materials.hpp"
#include "sandcraft_view.hpp"

namespace sandcraft {
namespace vg {

// ---- a stable hash ---------------------------------------------------------
inline uint32_t hashBits(int a, int b) {
  uint32_t h = (uint32_t(a) * 73856093u) ^ (uint32_t(b) * 19349663u);
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return h;
}
inline float hash01(int a, int b) {
  return float(hashBits(a, b) & 0xFFFFu) / 65535.0f;
}

// ---- colour arithmetic, on cv::Scalar's BGRA channel order -----------------
inline cv::Scalar mix(const cv::Scalar &a, const cv::Scalar &b, float t) {
  return cv::Scalar(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t,
                    a[2] + (b[2] - a[2]) * t, a[3]);
}
inline cv::Scalar withAlpha(const cv::Scalar &c, float a) {
  return cv::Scalar(c[0], c[1], c[2], a);
}
// Lead white, gesso, the cream of a star: the colour reached for whenever
// light itself is the subject.
inline cv::Scalar cream() { return cv::Scalar(226, 240, 250, 255); }
inline cv::Scalar cerulean() { return cv::Scalar(194, 144, 74, 255); }

// ---- the pigments ----------------------------------------------------------
// The palette Van Gogh actually worked from: chrome yellow, cadmium yellow and
// orange, vermilion, emerald green, viridian, Prussian blue, cobalt blue,
// ultramarine, cerulean, lead white, ivory black, burnt sienna and umber.
// Every entry carries light, base and dark -- the three passages a stroke is
// painted with -- and a complement, the thin accent that keeps a field of one
// colour from reading as flat.
struct Pigment {
  cv::Scalar light, base, dark, comp;
};

inline const std::array<Pigment, PARTICLE_TYPE_COUNT> &pigmentTable() {
  static const std::array<Pigment, PARTICLE_TYPE_COUNT> table = [] {
    std::array<Pigment, PARTICLE_TYPE_COUNT> p{};
    const auto rgb = [](int r, int g, int b) { return cv::Scalar(b, g, r, 255); };
    const auto set = [&p, &rgb](ParticleType t, int lr, int lg, int lb, int br,
                                int bg, int bb, int dr, int dg, int db) {
      const cv::Scalar base = rgb(br, bg, bb);
      // The complement is the channel swap: blue turns to orange, chrome
      // yellow to violet, vermilion to green. It is a crude complement, and at
      // the weight it is used it is the right one.
      p[t] = Pigment{rgb(lr, lg, lb), base, rgb(dr, dg, db),
                     cv::Scalar(base[2], base[1], base[0], 255)};
    };
    // In the palette's own order, so the two tables can be diffed.
    set(SAND, 245, 212, 97, 227, 169, 60, 162, 105, 31);    // chrome yellow, wheat
    set(WATER, 127, 178, 232, 44, 95, 200, 22, 38, 94);     // cobalt into ultramarine
    set(SALTWATER, 110, 159, 224, 36, 68, 158, 18, 30, 78);
    set(OIL, 138, 106, 74, 74, 46, 30, 36, 20, 12);         // burnt umber
    set(DIRT, 200, 162, 114, 139, 94, 52, 74, 48, 24);      // burnt sienna
    set(MUD, 138, 106, 69, 90, 58, 32, 46, 30, 14);         // umber
    set(SALT, 251, 246, 230, 232, 223, 198, 169, 156, 124); // lead white
    set(ICE, 242, 251, 254, 191, 230, 240, 111, 168, 196);  // cerulean
    set(PLANT, 143, 196, 106, 63, 122, 70, 30, 66, 39);     // viridian
    set(WALL, 184, 180, 200, 110, 110, 136, 46, 48, 80);    // stone, blue shadow
    set(IRONWALL, 159, 168, 200, 78, 88, 120, 35, 40, 66);  // slate, Prussian
    set(RUST, 208, 128, 80, 154, 74, 32, 82, 36, 14);       // vermilion
    set(TORCH, 255, 208, 138, 255, 138, 42, 176, 58, 14);   // cadmium orange
    set(STOVE, 110, 106, 120, 58, 56, 70, 22, 22, 30);      // ivory black
    set(EMBER, 255, 176, 96, 224, 74, 32, 122, 30, 10);
    set(FIRE, 255, 224, 138, 255, 122, 30, 194, 42, 8);     // cadmium yellow -> orange
    set(ELEC, 255, 255, 208, 255, 232, 58, 200, 168, 0);    // chrome yellow
    set(STEAM, 234, 246, 255, 168, 200, 220, 94, 122, 140);
    set(ACID, 232, 255, 138, 184, 232, 58, 94, 140, 30);    // a sickly chrome green
    set(WATERSPOUT, 143, 192, 240, 46, 98, 168, 20, 46, 92);
    set(SANDSPOUT, 248, 224, 144, 210, 160, 58, 126, 90, 22);
    set(SALTSPOUT, 255, 255, 255, 220, 210, 188, 154, 140, 112);
    set(OILSPOUT, 154, 122, 90, 90, 58, 36, 44, 26, 14);
    set(NIL, 106, 106, 138, 42, 42, 62, 14, 14, 24);        // the void: violet-black
    set(NOTHING, 40, 52, 96, 26, 34, 66, 14, 18, 40);       // the sky under the paint
    return p;
  }();
  return table;
}

inline Pigment pigmentOf(ParticleType t) { return pigmentTable()[t]; }

// ---- the flow of the brush -------------------------------------------------
// Broad bands of alternating direction read as wind combed across a
// wheatfield; a per-cell alternation reads as television static. The band is
// coarse enough that a whole stroke keeps its sense (a stroke spans at most
// brush.length cells), so a settled field combs into diagonal swipes rather
// than dissolving into per-cell hatch.
inline float flowAngle(int cx, int cy, float t) {
  const bool flip = (((cy / 5) + (cx / 11)) & 1) != 0;
  const float dir = flip ? 0.50f : -0.50f;
  return dir + 0.20f * std::sin(float(cy) * 0.050f + t * 0.21f) +
         0.16f * std::cos(float(cx) * 0.041f - t * 0.17f);
}

// ---- one stroke ------------------------------------------------------------
struct Stroke {
  float x0, y0, x1, y1; // the axis of the brush
  float w;              // the loaded width
  float bulge;          // the belly, along the normal
  float wob, wob2;      // the ragged edges, along the normal
};

// Appends a stroke to an already-open path as one closed sub-path.
//
// The direction is canonicalised first. NanoVG fills with a non-zero winding
// rule, so two sub-paths of opposite orientation *subtract* where they overlap:
// a heap of strokes that point both ways would punch holes in itself. Pointing
// every stroke the same way is what makes a pass union instead.
inline void appendStroke(const Stroke &s) {
  using namespace cv::v4d::nvg;
  float ax = s.x0, ay = s.y0, bx = s.x1, by = s.y1;
  if (bx < ax || (bx == ax && by < ay))
    std::swap(ax, bx), std::swap(ay, by);
  const float dx = bx - ax, dy = by - ay;
  const float len = std::sqrt(dx * dx + dy * dy) + 1e-4f;
  const float nx = -dy / len, ny = dx / len;
  const float hw = s.w * 0.5f;
  // The control points sit a third of the way along the axis and are pushed
  // along the normal by the belly, which is what bows the stroke.
  const float c1x = ax + dx * 0.33f + nx * s.bulge;
  const float c1y = ay + dy * 0.33f + ny * s.bulge;
  const float c2x = bx - dx * 0.33f + nx * s.bulge;
  const float c2y = by - dy * 0.33f + ny * s.bulge;
  moveTo(ax - nx * hw + nx * s.wob, ay - ny * hw + ny * s.wob);
  bezierTo(c1x - nx * hw, c1y - ny * hw, c2x - nx * hw, c2y - ny * hw,
           bx - nx * hw + nx * s.wob2, by - ny * hw + ny * s.wob2);
  bezierTo(c2x + nx * hw, c2y + ny * hw, c1x + nx * hw, c1y + ny * hw,
           ax + nx * hw + nx * s.wob, ay + ny * hw + ny * s.wob);
  closePath();
}

// ---- the four passes, batched ---------------------------------------------
// The strokes of one material are collected and filled afterwards, one path
// per (pass, variant). A heap of settled sand is a few thousand strokes, but
// it is still only a handful of draw calls, because NanoVG fills every
// sub-path of a path in one go -- and, with a non-zero winding, the strokes of
// one pass do not darken each other where they overlap.
enum Pass { SHADOW = 0, BODY = 1, LIGHT = 2, ACCENT = 3, kPasses = 4 };
inline constexpr int kVariants = 4;

struct Batch {
  std::array<std::array<std::vector<Stroke>, kVariants>, kPasses> pass{};
  void clear() {
    for (auto &p : pass)
      for (auto &v : p)
        v.clear();
  }
};

// The same pigment, pushed four ways: as it is, towards the light, towards the
// dark, and towards its own complement.
inline cv::Scalar variantColor(const Pigment &pig, int variant) {
  switch (variant) {
  case 0:
    return pig.base;
  case 1:
    return mix(pig.base, pig.light, 0.45f);
  case 2:
    return mix(pig.base, pig.dark, 0.45f);
  default:
    return mix(pig.base, pig.comp, 0.55f);
  }
}

// How a material is brushed. The automaton decides where a material is; the
// brush decides how it lies on the canvas.
struct Brush {
  float length = 5.0f;  // cells per stroke
  float width = 0.75f;  // stroke width, in cells
  float alpha = 250.0f; // body alpha
  float flow = 1.0f;   // how tightly the flow field steers the strokes
  float rise = 0.0f;   // extra vertical bias (fire climbs, steam drifts)
  float ground = 250.0f; // the imprimata under the strokes
};

// ---- the painter -----------------------------------------------------------
// One material, over its runs: the ground, then shadow, body, light and the
// accent of the complement, all filled in that order.
inline void paintMaterial(const Pigment &pig, const std::vector<Run> &runs,
                          const View &v, float tsec, const Brush &brush,
                          float impasto, Batch &batch) {
  using namespace cv::v4d::nvg;
  if (runs.empty())
    return;
  const float cell = v.cell_;
  batch.clear();

  // --- the ground: one wavy rectangle per run, opaque ----------------------
  {
    beginPath();
    for (const Run &r : runs) {
      const float x0 = v.origin_.x + r.x0_ * cell;
      const float x1 = v.origin_.x + (r.x1_ + 1) * cell;
      const float y0 = v.origin_.y + r.y_ * cell;
      const float y1 = y0 + cell;
      const float mid = (x0 + x1) * 0.5f;
      const float a = hash01(r.x0_, r.y_ + 7) - 0.5f;
      const float b = hash01(r.x0_ + 3, r.y_) - 0.5f;
      moveTo(x0, y0 + cell * 0.12f * a);
      quadTo(mid, y0 + cell * 0.24f * a, x1, y0 + cell * 0.12f * b);
      lineTo(x1, y1 - cell * 0.12f * b);
      quadTo(mid, y1 - cell * 0.22f * a, x0, y1 - cell * 0.12f * a);
      closePath();
    }
    fillColor(withAlpha(pig.base, brush.ground));
    fill();
  }

  // --- the strokes ---------------------------------------------------------
  for (const Run &r : runs) {
    const float x0 = v.origin_.x + r.x0_ * cell;
    const float x1 = v.origin_.x + (r.x1_ + 1) * cell;
    const float yc = v.origin_.y + (r.y_ + 0.5f) * cell;
    const float len = x1 - x0;
    if (len <= 0.25f)
      continue;
    const float cells = float(r.x1_ - r.x0_ + 1);
    const float h = hash01(r.x0_, r.y_);
    const float h2 = hash01(r.y_ + 11, r.x0_);
    // A long run is combed into several strokes; a short one is a single dab.
    const int strokes = std::clamp(
        int(std::floor(len / (cell * brush.length) * impasto + 0.5f)), 1, 4);
    const float w = cell * brush.width * (0.58f + 0.42f * h);
    for (int s = 0; s < strokes; ++s) {
      const float f0 = (float(s) + 0.18f) / float(strokes);
      const float f1 = (float(s) + 0.82f) / float(strokes);
      const float mid = x0 + len * (f0 + f1) * 0.5f;
      const int cx = int(float(r.x0_) + cells * (f0 + f1) * 0.5f);
      float ang = flowAngle(cx, r.y_, tsec) * brush.flow;
      ang += brush.rise;
      const float ca = std::cos(ang), sa = std::sin(ang);
      const float nx = -sa, ny = ca;
      const float half = std::max(len / float(strokes) * 0.42f, cell * 0.25f);
      const Stroke axis{mid - ca * half, yc - sa * half, mid + ca * half,
                        yc + sa * half, w, (h2 - 0.5f) * w * 1.4f,
                        (h - 0.5f) * cell * 0.16f, (h2 - 0.5f) * cell * 0.16f};
      const int variant =
          int(hash01(r.x0_ + s * 31, r.y_ + 5) * float(kVariants + 1)) %
          kVariants;

      // The underpainting: the shadow the stroke throws on what is below it.
      batch.pass[SHADOW][variant].push_back(
          Stroke{axis.x0 + nx * cell * 0.20f, axis.y0 + ny * cell * 0.20f,
                 axis.x1 + nx * cell * 0.20f, axis.y1 + ny * cell * 0.20f,
                 w * 1.10f, axis.bulge, axis.wob, axis.wob2});
      // The body.
      batch.pass[BODY][variant].push_back(axis);
      // The ridge of thick paint that catches the light.
      batch.pass[LIGHT][variant].push_back(
          Stroke{axis.x0 - nx * cell * 0.12f, axis.y0 - ny * cell * 0.12f,
                 axis.x1 - nx * cell * 0.12f, axis.y1 - ny * cell * 0.12f,
                 w * 0.40f, axis.bulge, axis.wob * 0.5f, axis.wob2 * 0.5f});
      // And one thin note of the complement along the stroke's far edge, laid
      // on only when there is paint to spare.
      if (impasto >= 1.0f)
        batch.pass[ACCENT][variant].push_back(
            Stroke{axis.x0 + nx * cell * 0.32f, axis.y0 + ny * cell * 0.32f,
                   axis.x1 + nx * cell * 0.32f, axis.y1 + ny * cell * 0.32f,
                   cell * 0.16f, axis.bulge, 0.0f, 0.0f});
    }
  }

  // --- fill the four passes, dark to light ---------------------------------
  for (int pass = 0; pass < kPasses; ++pass) {
    for (int variant = 0; variant < kVariants; ++variant) {
      const auto &strokes = batch.pass[pass][variant];
      if (strokes.empty())
        continue;
      cv::Scalar col = pig.base;
      float alpha = brush.alpha;
      switch (pass) {
      case SHADOW:
        col = mix(pig.dark, pig.comp, 0.22f);
        alpha = 118.0f;
        break;
      case BODY:
        col = variantColor(pig, variant);
        alpha = brush.alpha;
        break;
      case LIGHT:
        col = mix(pig.light, cream(), 0.35f);
        alpha = 138.0f;
        break;
      default:
        col = pig.comp;
        alpha = 96.0f;
        break;
      }
      beginPath();
      for (const Stroke &s : strokes)
        appendStroke(s);
      fillColor(withAlpha(col, alpha));
      fill();
    }
  }
}

// ---- the glare -------------------------------------------------------------
// The other half of the look: light that does not merely reflect but blooms
// out of the canvas. NanoVG's NVG_LIGHTER composites additively, which is
// exactly what a lit oil surface does -- Van Gogh's fire has no edge.
inline void glare(const cv::Scalar &col, float cx, float cy, float r,
                  float alpha) {
  using namespace cv::v4d::nvg;
  if (r <= 0.0f || alpha <= 0.0f)
    return;
  beginPath();
  circle(cx, cy, r);
  fillPaint(radialGradient(cx, cy, 0.0f, r, withAlpha(col, alpha),
                           cv::Scalar(0, 0, 0, 0)));
  fill();
}

inline void glareRun(const cv::Scalar &col, const std::vector<Run> &runs,
                     const View &v, float radius, float alpha, float tsec) {
  using namespace cv::v4d::nvg;
  save();
  globalCompositeOperation(NVG_LIGHTER);
  for (const Run &r : runs) {
    // A hash of the cell and the frame *quantum* rather than a re-roll every
    // frame: fire flickers at 12 Hz, smoothly, instead of boiling.
    const float phase = tsec * 12.0f;
    const float q = std::floor(phase);
    const float k = phase - q;
    const float a = 0.72f + 0.28f * hash01(r.x0_ * 2 + int(q), r.y_);
    const float b = 0.72f + 0.28f * hash01(r.x0_ * 2 + int(q) + 1, r.y_);
    const float flick = a + (b - a) * k;
    glare(col, v.origin_.x + (r.x0_ + r.x1_ + 1) * 0.5f * v.cell_,
          v.origin_.y + (r.y_ + 0.5f) * v.cell_, radius, alpha * flick);
  }
  restore();
}

// ---- the canvas ------------------------------------------------------------
// A tileable linen weave, generated once and uploaded as one NanoVG image:
// two families of coarse thread running across each other, with the fibre
// noise of real cloth. It is the difference between a painting on canvas and a
// picture on a screen, and it costs one image fill.
inline int weaveImage() {
  // One per process. The node that reads it executes on a single worker
  // thread, and the image lives in the NanoVG context, not in the CPU cache.
  static int handle = -1;
  if (handle < 0) {
    constexpr int N = 256;
    cv::Mat weave(N, N, CV_8UC4);
    for (int y = 0; y < N; ++y) {
      for (int x = 0; x < N; ++x) {
        // Periods of 64 px, four times over the tile, so the weave repeats.
        const float th = 0.5f + 0.5f * std::sin(float(y) * 0.0981748f);
        const float tv = 0.5f + 0.5f * std::sin(float(x) * 0.0981748f + 1.7f);
        // Fibre noise on a 16 px lattice, also periodic over the tile.
        const float n = hash01(x & 15, y & 15);
        const float shade = 0.30f * th + 0.30f * tv + 0.40f * n;
        weave.at<cv::Vec4b>(y, x) =
            cv::Vec4b(232, 224, 205, uint8_t(std::clamp(shade, 0.0f, 1.0f) *
                                             150.0f));
      }
    }
    handle = cv::v4d::nvg::createImageRGBA(
        N, N, NVG_IMAGE_REPEATX | NVG_IMAGE_REPEATY, weave.data);
  }
  return handle;
}

// The canvas, over a rectangle: the weave at a given strength.
inline void paintCanvas(float x, float y, float w, float h, float strength) {
  using namespace cv::v4d::nvg;
  if (w <= 0.0f || h <= 0.0f)
    return;
  const int img = weaveImage();
  if (img < 0)
    return;
  beginPath();
  rect(x, y, w, h);
  fillPaint(imagePattern(x, y, x + w, y + h, 0.0f, img, strength));
  fill();
}

// ---- the night -------------------------------------------------------------
// Starry Night, as the empty cells of the world: a field of ultramarine with
// three vortices rolled through it and eleven halated stars. Van Gogh painted
// eleven -- ten stars and the moon -- so eleven it is.
inline void paintNight(float x, float y, float w, float h, float cell,
                       float tsec) {
  using namespace cv::v4d::nvg;

  // The field: a vertical wash from Prussian blue at the top of the slab to
  // near-black at the bottom.
  beginPath();
  rect(x, y, w, h);
  fillPaint(linearGradient(x, y, x, y + h, cv::Scalar(150, 104, 46, 255),
                           cv::Scalar(40, 26, 12, 255)));
  fill();

  // Three vortices, rolled as short spiral ribbons of cerulean.
  static const float kCol[3] = {0.24f, 0.62f, 0.86f};
  static const float kRow[3] = {0.30f, 0.68f, 0.44f};
  for (int k = 0; k < 3; ++k) {
    const float vx = x + w * kCol[k], vy = y + h * kRow[k];
    const float R = std::min(w, h) * (0.20f + 0.11f * k);
    const float spin = 0.05f + 0.02f * float(k);
    for (int i = 0; i < 22; ++i) {
      const float t = float(i) / 22.0f;
      // The golden angle: each segment starts where the last left off, which is
      // what makes a spiral rather than a wreath.
      const float a0 = 2.399963f * float(i) + tsec * spin;
      const float r0 = R * (0.12f + 0.88f * t);
      const float r1 = R * (0.12f + 0.88f * (t + 0.07f));
      const float a1 = a0 + 0.62f;
      Stroke s;
      s.x0 = vx + r0 * std::cos(a0);
      s.y0 = vy + r0 * std::sin(a0);
      s.x1 = vx + r1 * std::cos(a1);
      s.y1 = vy + r1 * std::sin(a1);
      s.w = std::max(1.5f, cell * (0.52f + 0.85f * t));
      s.bulge = R * 0.06f * std::sin(a0 * 2.0f);
      s.wob = (hash01(i, k) - 0.5f) * s.w * 0.3f;
      s.wob2 = (hash01(k, i) - 0.5f) * s.w * 0.3f;
      beginPath();
      appendStroke(s);
      fillColor(withAlpha(mix(cerulean(), cream(), 0.30f + 0.45f * t),
                           62.0f + 30.0f * t));
      fill();
    }
  }

  // Eleven stars, each a core, a halo and seven rays.
  for (int i = 0; i < 11; ++i) {
    const float sx = x + w * (0.05f + 0.90f * hash01(i, 101));
    const float sy = y + h * (0.04f + 0.38f * hash01(i, 202));
    const float rad = cell * (4.5f + 9.0f * hash01(i, 303));
    const float tw = 0.55f + 0.45f * std::sin(tsec * 1.6f + float(i) * 2.1f);
    save();
    globalCompositeOperation(NVG_LIGHTER);
    glare(mix(cream(), cv::Scalar(30, 170, 235, 255), 0.45f), sx, sy, rad * 2.2f,
          140.0f * tw);
    glare(cream(), sx, sy, rad * 0.55f, 235.0f * tw);
    constexpr int kRays = 7;
    for (int k = 0; k < kRays; ++k) {
      const float a = 6.2831853f * float(k) / float(kRays) + float(i);
      Stroke s;
      s.x0 = sx + std::cos(a) * rad * 0.5f;
      s.y0 = sy + std::sin(a) * rad * 0.5f;
      s.x1 = sx + std::cos(a) * rad * 1.9f;
      s.y1 = sy + std::sin(a) * rad * 1.9f;
      s.w = std::max(1.0f, rad * 0.16f);
      s.bulge = 0.0f;
      s.wob = 0.0f;
      s.wob2 = 0.0f;
      beginPath();
      appendStroke(s);
      fillColor(withAlpha(cream(), 110.0f * tw));
      fill();
    }
    restore();
  }
}

// ---- the varnish -----------------------------------------------------------
// The last thing on the canvas: a vignette, the way a painting darkens at the
// edges where the light does not fall straight on it.
inline void paintVignette(const cv::Size &sz, float strength) {
  using namespace cv::v4d::nvg;
  const float cx = sz.width * 0.5f, cy = sz.height * 0.5f;
  const float r = std::sqrt(cx * cx + cy * cy) + 1.0f;
  beginPath();
  rect(0.0f, 0.0f, float(sz.width), float(sz.height));
  fillPaint(radialGradient(cx, cy, r * 0.42f, r * 1.06f, cv::Scalar(0, 0, 0, 0),
                           withAlpha(cv::Scalar(10, 7, 3, 255), strength)));
  fill();
}

} // namespace vg
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_VANGOGH_HPP_
