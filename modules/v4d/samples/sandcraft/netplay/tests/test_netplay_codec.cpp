// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The netplay codec self-test (roadmap M1). A standalone executable, not a
// gtest case: it links the netplay headers and zstd and nothing from V4D or
// OpenCV, which is what "prove the codec without sockets" means in practice.
// It returns non-zero if any check fails, so the build/headless tooling can
// judge it on the exit code alone.
//
// The tests are the specification's acceptance criteria, not an implementation
// detail:
//   * decompress(compress(x)) == x for sizes 0, 1 and 640*360 (spec §14.3);
//   * a delta applied to the wrong base is rejected (roadmap M1);
//   * a corrupt delta leaves the replica untouched (spec §18.2);
//   * a real-ish frame's byte counts and encode/decode time are reported so the
//     spec's §12.5 estimate can be replaced by a measurement.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "grid_codec.hpp"

using sandcraft::netplay::applyDelta;
using sandcraft::netplay::applyKeyframe;
using sandcraft::netplay::Blob;
using sandcraft::netplay::crc32;
using sandcraft::netplay::Grid;
using sandcraft::netplay::makeDelta;
using sandcraft::netplay::makeKeyframe;
using sandcraft::netplay::xorDiff;
using sandcraft::netplay::ZstdCodec;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      ++g_failures;                                                            \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
    }                                                                          \
  } while (0)

static Grid randomGrid(std::mt19937 &rng, size_t n, unsigned materialCount = 36) {
  std::uniform_int_distribution<unsigned> churn(0, 9);
  std::uniform_int_distribution<unsigned> mat(0, materialCount - 1);
  Grid g(n, 0);
  for (size_t i = 0; i < n; ++i)
    if (churn(rng) == 0)
      g[i] = uint8_t(mat(rng));
  return g;
}

static Grid seededFrame() {
  // 640x360, the demo's grid (spec §7.1): a wall floor, a sand bank, a water
  // pool and a little noise -- a frame that is mostly settled, which is what a
  // delta actually sees.
  const int w = 640, h = 360;
  Grid g(size_t(w) * h, 0);
  auto put = [&](int x, int y, uint8_t v) {
    if (x >= 0 && y >= 0 && x < w && y < h)
      g[size_t(y) * w + x] = v;
  };
  for (int x = 0; x < w; ++x) {
    put(x, h - 2, 8); // WALL-ish
    put(x, h - 3, 8);
  }
  for (int x = w / 8; x < w / 3; ++x)
    for (int y = h - 12; y < h - 4; ++y)
      put(x, y, 2); // SAND
  for (int x = 5 * w / 8; x < 9 * w / 10; ++x)
    for (int y = h - 12; y < h - 4; ++y)
      put(x, y, 4); // WATER
  std::mt19937 rng(1234);
  for (int i = 0; i < 2000; ++i) {
    const int x = int(rng() % unsigned(w)), y = int(rng() % unsigned(h - 4));
    put(x, y, uint8_t(2 + (rng() % 6)));
  }
  return g;
}

int main() {
  ZstdCodec z;
  CHECK(z.ok());

  // --- crc32: the standard check value -------------------------------------
  {
    const char *s = "123456789";
    CHECK(crc32(reinterpret_cast<const uint8_t *>(s), 9) == 0xCBF43926u);
    CHECK(crc32(nullptr, 0) == 0x00000000u);
  }

  // --- xor is its own inverse ----------------------------------------------
  {
    std::mt19937 rng(7);
    Grid a = randomGrid(rng, 4096), b = randomGrid(rng, 4096);
    Grid d = xorDiff(a, b);
    Grid c = a;
    CHECK(sandcraft::netplay::applyXor(c, d));
    CHECK(c == b);
  }

  // --- keyframe round trip, including empty and single-cell -----------------
  for (const size_t n : {size_t(0), size_t(1), size_t(640 * 360)}) {
    std::mt19937 rng(11);
    Grid in = randomGrid(rng, n);
    Blob kf = makeKeyframe(z, in);
    CHECK(kf.rawLen == n);
    CHECK(kf.crc == crc32(in));
    Grid out;
    CHECK(applyKeyframe(z, kf, out));
    CHECK(out == in);
  }

  // --- keyframe checksum rejects corruption ---------------------------------
  {
    std::mt19937 rng(13);
    Grid in = randomGrid(rng, 65536);
    Blob kf = makeKeyframe(z, in);
    kf.crc ^= 0x1u; // a bit-flipped checksum must be caught
    Grid out;
    CHECK(!applyKeyframe(z, kf, out));
  }

  // --- delta round trip -----------------------------------------------------
  {
    std::mt19937 rng(17);
    Grid base = randomGrid(rng, 640 * 360);
    Grid cur = base;
    // touch a few hundred cells, as a few frames of painting would
    for (int i = 0; i < 500; ++i)
      cur[size_t(rng() % cur.size())] = uint8_t(rng() % 36);
    Blob d = makeDelta(z, base, cur);
    CHECK(d.rawLen == cur.size());
    CHECK(d.crc == crc32(cur));
    Grid dst = base;
    CHECK(applyDelta(z, dst, d));
    CHECK(dst == cur);
  }

  // --- a delta applied to the wrong base is detected and does not corrupt ---
  {
    std::mt19937 rng(19);
    Grid base = randomGrid(rng, 640 * 360);
    Grid cur = base;
    cur[123] = uint8_t(cur[123] ^ 5u);
    Blob d = makeDelta(z, base, cur);
    Grid wrong = randomGrid(rng, 640 * 360);
    Grid before = wrong;
    CHECK(!applyDelta(z, wrong, d));
    CHECK(wrong == before); // rolled back
  }

  // --- a corrupt delta is rejected and leaves the replica untouched ---------
  {
    std::mt19937 rng(23);
    Grid base = randomGrid(rng, 640 * 360);
    Grid cur = base;
    cur[7] = 9;
    Blob d = makeDelta(z, base, cur);
    // Flip a byte in the middle of the compressed stream, if there is one.
    if (!d.comp.empty())
      d.comp[d.comp.size() / 2] ^= 0xFFu;
    Grid dst = base;
    Grid before = dst;
    const bool ok = applyDelta(z, dst, d);
    // Either zstd rejects it or the checksum does; what must not happen is a
    // silently wrong grid.
    CHECK(!ok || dst == cur);
    if (!ok)
      CHECK(dst == before);
  }

  // --- measurement on a representative frame (spec §12.5) -------------------
  {
    std::mt19937 rng(29);
    Grid base = seededFrame();
    Grid cur = base;
    // ~150 brush stamps of a 3x3 disc, the kind of change a second of play makes
    std::uniform_int_distribution<int> px(8, 640 - 8), py(8, 360 - 12);
    for (int i = 0; i < 150; ++i) {
      const int cx = px(rng), cy = py(rng);
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
          cur[size_t(cy + dy) * 640 + (cx + dx)] = uint8_t(2 + (rng() % 4));
    }

    auto t0 = std::chrono::steady_clock::now();
    Blob kf = makeKeyframe(z, cur);
    auto t1 = std::chrono::steady_clock::now();
    Blob d = makeDelta(z, base, cur);
    auto t2 = std::chrono::steady_clock::now();

    Grid kout, dout = base;
    auto t3 = std::chrono::steady_clock::now();
    CHECK(applyKeyframe(z, kf, kout));
    CHECK(kout == cur);
    auto t4 = std::chrono::steady_clock::now();
    CHECK(applyDelta(z, dout, d));
    CHECK(dout == cur);
    auto t5 = std::chrono::steady_clock::now();

    const auto us = [](auto a, auto b) {
      return std::chrono::duration<double, std::micro>(b - a).count();
    };
    std::printf("frame 640x360 raw=%zu  keyframe=%zu B (%.1f%%)  delta=%zu B "
                "(%.1f%%)\n",
                size_t(640 * 360), kf.comp.size(),
                100.0 * double(kf.comp.size()) / double(640 * 360),
                d.comp.size(), 100.0 * double(d.comp.size()) / double(640 * 360));
    std::printf("times: keyframe enc=%.0f us dec=%.0f us   delta enc=%.0f us "
                "dec=%.0f us\n",
                us(t0, t1), us(t3, t4), us(t1, t2), us(t4, t5));
  }

  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "ok",
              g_checks, g_failures);
  return g_failures ? 1 : 0;
}
