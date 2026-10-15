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
#include <opencv2/imgproc.hpp>
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

// ---- value noise, for the sky ---------------------------------------------
// The sky's flow field needs a *smooth* 2D field, not the uncorrelated hash
// above: neighbouring samples have to agree or the strokes tear instead of
// flowing. Bilinear value noise with a smoothstep is the cheapest thing that
// does; the hash is still the source of the random corners, so the two agree
// about what "random" means.
inline float vnoise2(float x, float y) {
  const int xi = int(std::floor(x)), yi = int(std::floor(y));
  const float xf = x - float(xi), yf = y - float(yi);
  const float u = xf * xf * (3.0f - 2.0f * xf);
  const float v = yf * yf * (3.0f - 2.0f * yf);
  const float a = hash01(xi, yi), b = hash01(xi + 1, yi);
  const float c = hash01(xi, yi + 1), d = hash01(xi + 1, yi + 1);
  return (a * (1.0f - u) + b * u) * (1.0f - v) + (c * (1.0f - u) + d * u) * v;
}
inline float fbm2(float x, float y, int octaves) {
  float s = 0.0f, a = 0.5f, f = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    s += a * vnoise2(x * f, y * f);
    f *= 2.03f;
    a *= 0.5f;
  }
  return s;
}

// ---- the sky's flow --------------------------------------------------------
// A unit vector at a point of the *normalised* slab (u,v in [0,1]), so the same
// painting appears whatever the window size. Three things are added, and the
// sum is what makes it read as Van Gogh rather than as generic flow art:
//
//   * the curl of a value-noise field -- (dN/dy, -dN/dx) -- which is
//     divergence-free, so the streamlines swirl and never cross or focus to a
//     point (a plain gradient would make them all pile into the noise minima);
//   * two explicit vortices, the great spirals of the painting, falling off
//     with a Gaussian so each turns only its own corner of the sky;
//   * a travelling sine across the sky, the broad rolling wave the whole
//     composition rides on.
enum class SkyLayer { FIELD, HIGHLIGHT };
inline cv::Point2f skyVel(float u, float v, float t) {
  cv::Point2f f(0.0f, 0.0f);
  const float e = 0.014f;
  const float dnv =
      fbm2(u * 2.6f, (v + e) * 2.6f, 3) - fbm2(u * 2.6f, (v - e) * 2.6f, 3);
  const float dnu =
      fbm2((u + e) * 2.6f, v * 2.6f, 3) - fbm2((u - e) * 2.6f, v * 2.6f, 3);
  f.x += dnv / (2.0f * e) * 0.14f;
  f.y += -dnu / (2.0f * e) * 0.14f;
  const auto swirl = [&](float cx, float cy, float r, float s) {
    const float dx = u - cx, dy = v - cy;
    const float w = s * std::exp(-(dx * dx + dy * dy) / (2.0f * r * r));
    f.x += -dy * w;
    f.y += dx * w;
  };
  swirl(0.36f, 0.44f, 0.30f, 3.4f);
  swirl(0.66f, 0.26f, 0.22f, -2.8f);
  f.x += 1.0f;
  f.y += 0.85f *
         std::sin(6.2831853f * (v * 1.8f + 0.10f * std::sin(6.2831853f * u * 1.15f)) +
                  t * 0.10f);
  const float m = std::sqrt(f.x * f.x + f.y * f.y) + 1e-6f;
  return {f.x / m, f.y / m};
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
      // The floor on the half-length is what turns a falling grain -- a
      // one-cell run -- into a loaded dash rather than a dot. Without it the
      // streams of sand read as a stipple of single pixels, which is exactly
      // the "one SDL pixel per particle" look the painting is meant to leave
      // behind; with it, each grain is a stroke along the flow and a falling
      // column combs into a cascade of dashes.
      const float half =
          std::max(len / float(strokes) * 0.42f,
                   cell * std::max(0.5f, brush.length * 0.38f));
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
//
// Starry Night, as the background the world hangs in. Van Gogh's sky is not a
// gradient with stars on it: it is a *field of brush strokes* -- tens of
// thousands of short, loaded dashes, every one following the turbulent flow of
// the composition -- so the only honest way to paint it is to lay those strokes
// down. That is far more work than belongs in a frame (the sky never changes;
// only the sand over it does), so it is rasterised once, on the CPU, into a
// texture and drawn back with a single imagePattern -- the same route the
// canvas weave takes, and never fb().
//
// The raster is built at the slab's own pixel size and cached per size, so a
// resize repaints it once and every frame in between is one textured quad.

// The sky's pigments, in RGB because they are written into a cv::Mat (NanoVG's
// BGRA convention does not reach a CPU buffer). Ultramarine to lead white, the
// sequence Van Gogh mixed his night from.
struct SkyPigment {
  unsigned char r, g, b;
};
inline const std::array<SkyPigment, 7> &skyPigments() {
  static const std::array<SkyPigment, 7> p = {{
      {14, 24, 58},    // deep ultramarine, nearly black
      {24, 42, 92},    // Prussian blue
      {36, 64, 136},   // ultramarine
      {48, 92, 172},   // cobalt
      {92, 140, 202},  // cerulean
      {156, 192, 228}, // pale blue
      {226, 234, 248}, // lead white
  }};
  return p;
}
inline cv::Scalar pigmentRgba(const SkyPigment &p) {
  return cv::Scalar(p.r, p.g, p.b, 255);
}
inline int skyIndex(float q) {
  if (q < 0.34f)
    return 0;
  if (q < 0.56f)
    return 1;
  if (q < 0.74f)
    return 2;
  if (q < 0.87f)
    return 3;
  if (q < 0.94f)
    return 4;
  if (q < 0.978f)
    return 5;
  return 6;
}

// Where a star sits, in slab-local pixels. Shared by the raster and the live
// twinkle so the two cannot drift apart. Eleven of them: Van Gogh painted ten
// stars and the moon.
inline void starSlot(int i, float w, float h, float &sx, float &sy, float &rad) {
  const float sc = std::min(w, h);
  sx = w * (0.06f + 0.88f * hash01(i, 101));
  sy = h * (0.05f + 0.34f * hash01(i, 202));
  rad = sc * (0.013f + 0.024f * hash01(i, 303));
}
inline void moonSlot(float w, float h, float &mx, float &my, float &mr) {
  mx = w * 0.86f;
  my = h * 0.11f;
  mr = std::min(w, h) * 0.045f;
}

static inline cv::Scalar rgbOf(const cv::Vec3b &c, int d = 0) {
  const auto cl = [d](int v) { return uchar(std::clamp(v + d, 0, 255)); };
  return cv::Scalar(cl(c[0]), cl(c[1]), cl(c[2]), 255);
}

// One tapered brush ribbon along a polyline: a wide dark body, a narrower mid,
// and a thin bright ridge nudged along the normal, which is what a loaded
// brush leaves. Opaque, like real paint -- the strokes overwrite each other.
inline void skyRib(cv::Mat &img, const std::vector<cv::Point> &p,
                   const cv::Vec3b &body, float w, float bright, int seed) {
  if (p.size() < 2)
    return;
  cv::polylines(img, p, false, rgbOf(body), std::max(1.0, double(w)),
                cv::LINE_AA);
  if (w > 2.4f)
    cv::polylines(img, p, false, rgbOf(body, int(12 + 14 * bright)),
                  std::max(1.0, double(w * 0.45)), cv::LINE_AA);
  const float dx = float(p.back().x - p.front().x);
  const float dy = float(p.back().y - p.front().y);
  const float m = std::sqrt(dx * dx + dy * dy) + 1e-4f;
  const float nx = -dy / m, ny = dx / m;
  const float off = (hash01(seed, 91) - 0.35f) * w * 0.5f;
  std::vector<cv::Point> hp;
  hp.reserve(p.size());
  for (const cv::Point &q : p)
    hp.push_back(cv::Point(cvRound(q.x + nx * off), cvRound(q.y + ny * off)));
  cv::polylines(img, hp, false, rgbOf(body, int(40 + 52 * bright)),
                std::max(1.0, double(w * 0.22)), cv::LINE_AA);
}

// A flow-combed layer of strokes over the whole slab. Seeds sit on a jittered
// grid -- a regular grid reads as a printed hatch -- and each walks the sky's
// flow for a few segments, so neighbouring strokes share a direction and the
// field combs into the great swirls.
inline void skyStrokeLayer(cv::Mat &img, int W, int H, float t, float spacing,
                           float lenScale, int seedOff, SkyLayer layer) {
  const auto &pal = skyPigments();
  const int step = std::max(4, int(spacing));
  for (int gy = -step; gy < H + step; gy += step) {
    for (int gx = -step; gx < W + step; gx += step) {
      const int sx = gx + seedOff, sy = gy + seedOff * 3;
      const float x0 = gx + (hash01(sx, sy) - 0.5f) * float(step);
      const float y0 = gy + (hash01(sy, sx) - 0.5f) * float(step);
      float x = x0, y = y0;
      const float u0 = std::clamp(x0 / W, 0.0f, 1.0f);
      const float v0 = std::clamp(y0 / H, 0.0f, 1.0f);
      const float len =
          step * lenScale * (0.7f + 0.7f * hash01(sx + 7, sy + 3));
      constexpr int kSeg = 6;
      const float dl = len / float(kSeg);
      std::vector<cv::Point> pts;
      pts.reserve(kSeg + 1);
      for (int i = 0; i <= kSeg; ++i) {
        pts.push_back(cv::Point(cvRound(x), cvRound(y)));
        const cv::Point2f f =
            skyVel(std::clamp(x / W, 0.0f, 1.0f),
                   std::clamp(y / H, 0.0f, 1.0f), t);
        x += f.x * dl;
        y += f.y * dl;
      }
      float q = (fbm2(u0 * 2.4f + 3.1f, v0 * 2.4f - 1.7f, 3) - 0.20f) / 0.52f;
      const auto core = [&](float cx, float cy, float r) {
        const float dx = u0 - cx, dy = v0 - cy;
        return std::exp(-(dx * dx + dy * dy) / (2.0f * r * r));
      };
      q += 0.13f * core(0.36f, 0.44f, 0.24f) + 0.10f * core(0.66f, 0.26f, 0.18f);
      q = std::clamp(q, 0.0f, 1.0f);
      int idx = skyIndex(q);
      if (layer == SkyLayer::HIGHLIGHT) {
        // the light end of the palette only, sparser and thinner: the passages
        // of pale blue and white that rim the swirls.
        if (idx < 4)
          continue;
        idx = std::min(6, idx + 1);
      } else if (idx > 5) {
        idx = 5;
      }
      const float wdt = spacing * (0.30f + 0.28f * hash01(sx + 13, sy + 5)) *
                        (layer == SkyLayer::HIGHLIGHT ? 0.6f : 1.0f);
      skyRib(img, pts, cv::Vec3b(pal[idx].r, pal[idx].g, pal[idx].b),
             std::max(1.5f, wdt), 0.6f + 0.7f * hash01(sx, sy + 17), sx + sy);
    }
  }
}

// The hillside, the village and the cypress: the dark land the sky sits over,
// painted as opaque silhouettes with their own strokes so they hold up against
// the swirl rather than reading as flat cut-outs.
inline void skyPaintLand(cv::Mat &img, int W, int H) {
  const auto ridgeY = [&](float v) {
    return H * 0.80f + 0.055f * H * std::sin(v * 6.6f + 1.1f) +
           0.028f * H * std::sin(v * 15.0f + 0.3f);
  };
  // the hillside
  std::vector<cv::Point> hill;
  hill.push_back(cv::Point(0, H));
  for (int x = 0; x <= W; x += 3)
    hill.push_back(cv::Point(x, int(ridgeY(float(x) / float(W)))));
  hill.push_back(cv::Point(W, H));
  cv::fillPoly(img, std::vector<std::vector<cv::Point>>{hill},
               cv::Scalar(20, 34, 32, 255));
  for (int x = 0; x <= W; x += 9) {
    const float y = ridgeY(float(x) / float(W));
    const float len = 22.0f + 30.0f * hash01(x, 3);
    cv::line(img, cv::Point(x, int(y)), cv::Point(int(x + len), int(y - 6.0f)),
             cv::Scalar(34, 58, 50, 255), 3, cv::LINE_AA);
  }
  // the village, along the ridge: rooftops, a few lit windows, a spire
  const int baseY = int(H * 0.845f);
  for (int i = 0; i < 26; ++i) {
    const int x = int((0.30f + 0.42f * hash01(i, 11)) * W);
    const int hw = 8 + int(16 * hash01(i, 21));
    const int hh = 10 + int(22 * hash01(i, 31));
    std::vector<cv::Point> house = {
        cv::Point(x - hw, baseY), cv::Point(x + hw, baseY),
        cv::Point(x + hw, baseY - hh), cv::Point(x, baseY - hh - hw / 2),
        cv::Point(x - hw, baseY - hh)};
    cv::fillPoly(img, std::vector<std::vector<cv::Point>>{house},
                 cv::Scalar(16, 22, 40, 255));
    if (hash01(i, 41) > 0.45f)
      cv::rectangle(img, cv::Point(x - hw / 2, baseY - hh + 3),
                    cv::Point(x - hw / 2 + 3, baseY - hh + 6),
                    cv::Scalar(236, 196, 96, 255), -1);
  }
  const int cx = int(0.46f * W);
  cv::fillPoly(img,
               std::vector<std::vector<cv::Point>>{{cv::Point(cx - 7, baseY),
                                                    cv::Point(cx + 7, baseY),
                                                    cv::Point(cx, baseY - 62)}},
               cv::Scalar(14, 18, 34, 255));
  // the cypress, a flame of near-black green rising from the foreground
  const float ccx = 0.075f * W, cbase = H * 0.995f, ctop = H * 0.05f;
  std::vector<cv::Point> cyp;
  constexpr int kN = 40;
  for (int i = 0; i <= kN; ++i) {
    const float k = float(i) / kN;
    const float y = cbase + (ctop - cbase) * k;
    const float half = (0.055f * W) * std::pow(1.0f - k, 0.8f) + 0.004f * W;
    cyp.push_back(cv::Point(
        int(ccx + half + (1.0f - k) * 0.012f * W * std::sin(k * 22.0f + 1.0f)),
        int(y)));
  }
  for (int i = kN; i >= 0; --i) {
    const float k = float(i) / kN;
    const float y = cbase + (ctop - cbase) * k;
    const float half = (0.055f * W) * std::pow(1.0f - k, 0.8f) + 0.004f * W;
    cyp.push_back(cv::Point(
        int(ccx - half + (1.0f - k) * 0.012f * W * std::sin(k * 19.0f + 3.0f)),
        int(y)));
  }
  cv::fillPoly(img, std::vector<std::vector<cv::Point>>{cyp},
               cv::Scalar(9, 18, 13, 255));
  for (int i = 0; i < 340; ++i) {
    const float k = std::pow(hash01(i, 7), 0.7f);
    const float y = cbase + (ctop - cbase) * k;
    const float half = (0.055f * W) * std::pow(1.0f - k, 0.8f) + 0.004f * W;
    const float fx = ccx + (hash01(i, 13) - 0.5f) * 2.0f * half;
    const float len = (0.03f + 0.06f * hash01(i, 19)) * H;
    const float lean = (hash01(i, 23) - 0.5f) * 0.5f;
    std::vector<cv::Point> pts = {
        cv::Point(cvRound(fx), cvRound(y)),
        cv::Point(cvRound(fx + lean * len * 0.5f), cvRound(y - len * 0.5f)),
        cv::Point(cvRound(fx + lean * len), cvRound(y - len))};
    const cv::Vec3b c(uchar(10 + int(18 * hash01(i, 29))),
                      uchar(30 + int(38 * hash01(i, 31))),
                      uchar(18 + int(20 * hash01(i, 37))));
    cv::polylines(img, pts, false, rgbOf(c),
                  2.0 + 2.0 * hash01(i, 41), cv::LINE_AA);
  }
}

// Eleven halated stars and the crescent moon, painted into the raster. A star
// is a loaded dab of warm yellow inside three rows of short curved strokes --
// Van Gogh's halo is brushed, not printed -- and the moon is a cream disc with
// a bite taken out of it by a second, sky-coloured disc.
inline void skyPaintStars(cv::Mat &img, int W, int H) {
  const float sc = float(std::min(W, H));
  for (int i = 0; i < 11; ++i) {
    float sx, sy, rad;
    starSlot(i, float(W), float(H), sx, sy, rad);
    for (int ring = 3; ring >= 1; --ring) {
      const float rr = rad * (0.95f + 0.55f * float(ring));
      const int segs = 10 + ring * 4;
      for (int s = 0; s < segs; ++s) {
        const float a =
            6.2831853f * (float(s) + 0.5f * hash01(i * 31 + s, ring)) / segs;
        const float span = 0.14f + 0.20f * hash01(i * 7 + s, ring + 50);
        const float rr2 = rr * (0.90f + 0.18f * hash01(i + s, ring + 9));
        std::vector<cv::Point> arc;
        for (int k = 0; k <= 5; ++k) {
          const float aa =
              a + (span * 6.2831853f) * (float(k) / 5.0f - 0.5f);
          const float rk =
              rr2 * (1.0f - 0.10f * std::sin(float(k) / 5.0f * 3.14159f));
          arc.push_back(cv::Point(cvRound(sx + std::cos(aa) * rk),
                                  cvRound(sy + std::sin(aa) * rk)));
        }
        const cv::Vec3b c =
            (ring == 1) ? cv::Vec3b(244, 214, 138)
                        : (ring == 2 ? cv::Vec3b(190, 202, 222)
                                     : cv::Vec3b(140, 174, 212));
        const float wid = sc * 0.0032f * (0.6f + 0.9f * hash01(i * 3 + s, ring + 77));
        cv::polylines(img, arc, false, rgbOf(c), std::max(1.6, double(wid)),
                      cv::LINE_AA);
      }
    }
    for (int s = 0; s < 6; ++s) {
      const float a = 6.2831853f * float(s) / 6.0f + float(i) * 0.7f;
      const float r0 = rad * (1.15f + 0.12f * hash01(i, s));
      const float r1 = rad * (1.9f + 0.5f * hash01(i + 5, s + 9));
      std::vector<cv::Point> ray = {
          cv::Point(cvRound(sx + std::cos(a) * r0), cvRound(sy + std::sin(a) * r0)),
          cv::Point(cvRound(sx + std::cos(a) * r1), cvRound(sy + std::sin(a) * r1))};
      cv::polylines(img, ray, false, cv::Scalar(224, 216, 176, 255),
                    std::max(1.6, sc * 0.0024), cv::LINE_AA);
    }
    cv::circle(img, cv::Point(int(sx), int(sy)), int(rad * 0.62),
               cv::Scalar(244, 212, 120, 255), -1, cv::LINE_AA);
    cv::circle(img, cv::Point(int(sx), int(sy)), int(rad * 0.36),
               cv::Scalar(255, 244, 196, 255), -1, cv::LINE_AA);
  }
  float mx, my, mr;
  moonSlot(float(W), float(H), mx, my, mr);
  for (int s = 0; s < 16; ++s) {
    const float a = 6.2831853f * float(s) / 16.0f;
    const float r0 = mr * 1.02f, r1 = mr * (1.25f + 0.22f * hash01(s, 5));
    std::vector<cv::Point> ray = {
        cv::Point(cvRound(mx + std::cos(a) * r0), cvRound(my + std::sin(a) * r0)),
        cv::Point(cvRound(mx + std::cos(a) * r1), cvRound(my + std::sin(a) * r1))};
    cv::polylines(img, ray, false, cv::Scalar(226, 210, 150, 255),
                  std::max(2.0, double(sc * 0.0035)), cv::LINE_AA);
  }
  cv::circle(img, cv::Point(int(mx), int(my)), int(mr),
             cv::Scalar(248, 228, 150, 255), -1, cv::LINE_AA);
  cv::circle(img, cv::Point(int(mx - mr * 0.45f), int(my - mr * 0.30f)),
             int(mr * 0.92), cv::Scalar(30, 44, 90, 255), -1, cv::LINE_AA);
}

// Paint the whole raster: ground, flow, hills, stars.
inline cv::Mat buildSkyRaster(int W, int H, float t) {
  cv::Mat img(H, W, CV_8UC4);
  // the ground: a vertical wash, brighter across the middle band, sinking to
  // near-black above the hills (that is the gradient the strokes sit on, and
  // the parts of it the flow leaves uncovered are what make the sky breathe).
  const cv::Vec3b top(26, 40, 82), mid(46, 80, 150), bot(12, 18, 42);
  for (int y = 0; y < H; ++y) {
    const float v = float(y) / float(std::max(1, H - 1));
    cv::Vec3b c;
    if (v < 0.45f) {
      const float k = v / 0.45f;
      c = cv::Vec3b(uchar(top[0] + (mid[0] - top[0]) * k),
                    uchar(top[1] + (mid[1] - top[1]) * k),
                    uchar(top[2] + (mid[2] - top[2]) * k));
    } else {
      const float k = (v - 0.45f) / 0.55f;
      c = cv::Vec3b(uchar(mid[0] + (bot[0] - mid[0]) * k),
                    uchar(mid[1] + (bot[1] - mid[1]) * k),
                    uchar(mid[2] + (bot[2] - mid[2]) * k));
    }
    img.row(y).setTo(cv::Scalar(c[0], c[1], c[2], 255));
  }
  const float fine = std::max(6.0f, float(std::min(W, H)) / 95.0f);
  const float coarse = std::max(9.0f, float(std::min(W, H)) / 38.0f);
  skyStrokeLayer(img, W, H, t, fine, 2.4f, 0, SkyLayer::FIELD);
  skyStrokeLayer(img, W, H, t, coarse, 1.7f, 1000, SkyLayer::HIGHLIGHT);
  skyPaintLand(img, W, H);
  skyPaintStars(img, W, H);
  return img;
}

// The rasterised sky as a NanoVG image, regenerated only when the slab's pixel
// size changes. thread_local for the same reason Batch is: with more than one
// worker each has its own NanoVG context, and an image handle belongs to the
// context that created it.
inline int skyImage(int W, int H, float t) {
  static thread_local int handle = -1;
  static thread_local cv::Size size(0, 0);
  if (handle >= 0 && size == cv::Size(W, H))
    return handle;
  if (handle >= 0)
    cv::v4d::nvg::deleteImage(handle);
  const cv::Mat img = buildSkyRaster(W, H, t);
  handle = cv::v4d::nvg::createImageRGBA(W, H, 0, img.data);
  size = cv::Size(W, H);
  return handle;
}

// Draw the sky over the slab, then let the painted stars breathe: a slow
// additive twinkle laid *over* the raster, which is the only part of the sky
// that moves, and enough to keep it alive without re-painting the swirl.
inline void paintSky(float x, float y, float w, float h, float tsec) {
  using namespace cv::v4d::nvg;
  if (w <= 0.0f || h <= 0.0f)
    return;
  const int W = std::max(1, int(w)), H = std::max(1, int(h));
  const int img = skyImage(W, H, tsec);
  if (img >= 0) {
    beginPath();
    rect(x, y, w, h);
    fillPaint(imagePattern(x, y, x + w, y + h, 0.0f, img, 1.0f));
    fill();
  }
  save();
  globalCompositeOperation(NVG_LIGHTER);
  for (int i = 0; i < 11; ++i) {
    float sx, sy, rad;
    starSlot(i, w, h, sx, sy, rad);
    const float tw = 0.55f + 0.45f * std::sin(tsec * 1.6f + float(i) * 2.1f);
    glare(mix(cream(), cv::Scalar(0, 190, 255, 255), 0.5f), x + sx, y + sy,
          rad * 1.9f, 34.0f * tw);
  }
  {
    float mx, my, mr;
    moonSlot(w, h, mx, my, mr);
    const float tw = 0.7f + 0.3f * std::sin(tsec * 0.9f);
    glare(cream(), x + mx, y + my, mr * 1.5f, 30.0f * tw);
  }
  restore();
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
