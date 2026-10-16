// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The world-state codec for netplay (specification.md §12.2, §14.2).
//
// The authoritative grid is one byte per cell, so a keyframe is the grid and a
// delta is the XOR of two grids. Falls-and-settles frames change locally, so the
// XOR is dominated by long zero runs and repeated byte patterns and zstd turns
// that into a small frame with no side metadata and nothing to desynchronise.
//
// A delta is only applicable to the exact grid it was diffed against, so the
// checksum is taken over the *resulting* grid (spec §12.2 allows the raw diff
// or the resulting grid; the result is the stronger of the two). The receiver
// XORs the diff into the replica in place, checksums the result, and because
// XOR is its own inverse rolls the replica back if the checksum disagrees. That
// detects both a corrupt diff and a delta applied to the wrong base, which is
// exactly the M1 acceptance criterion. A mismatch is the trigger for a RESYNC
// (spec §12.3), not a repair attempt.
//
// Everything here is pure -- it takes a ZstdCodec to borrow the contexts, and
// nothing else. That is what lets the codec be proven without sockets (roadmap
// guiding principle 2): the test target links this header and zstd and nothing
// from V4D.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_GRID_CODEC_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_GRID_CODEC_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "zstd_codec.hpp"

namespace sandcraft {
namespace netplay {

// The wire grid and the diff are the same shape: W*H bytes, row-major, one
// byte per cell (spec §7.1). A distinct alias keeps the codec honest about
// what it is handed.
using Grid = std::vector<uint8_t>;

// CRC-32, IEEE 802.3 (reflected polynomial 0xEDB88320, init/final 0xFFFFFFFF)
// -- the same one zlib/gzip use, implemented here so the codec needs no zlib.
// It is only ever compared against another value this function produced, so the
// choice of CRC variant is internal, not a compatibility surface.
inline const std::array<uint32_t, 256> &crc32Table() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  return table;
}

inline uint32_t crc32(const uint8_t *data, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  const std::array<uint32_t, 256> &t = crc32Table();
  for (size_t i = 0; i < n; ++i)
    c = t[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

inline uint32_t crc32(const Grid &g) { return crc32(g.data(), g.size()); }

// A compressed keyframe (raw == grid) or delta (raw == XOR diff), with the
// lengths and checksum the wire header will carry (spec §13.4).
struct Blob {
  std::vector<uint8_t> comp; // zstd frame
  uint32_t rawLen = 0;       // uncompressed length, == W*H for both kinds
  uint32_t crc = 0;          // keyframe: crc32(grid); delta: crc32(resulting grid)
};

// diff[i] = base[i] ^ cur[i]. Both grids must be the same length; a mismatch
// is a programming error, not a wire condition, and yields an empty diff.
inline Grid xorDiff(const Grid &base, const Grid &cur) {
  if (base.size() != cur.size())
    return {};
  Grid diff(base.size());
  for (size_t i = 0; i < base.size(); ++i)
    diff[i] = uint8_t(base[i] ^ cur[i]);
  return diff;
}

// dst[i] ^= diff[i], in place. Both must be the same length.
inline bool applyXor(Grid &dst, const Grid &diff) {
  if (dst.size() != diff.size())
    return false;
  for (size_t i = 0; i < dst.size(); ++i)
    dst[i] = uint8_t(dst[i] ^ diff[i]);
  return true;
}

// Full state: compress the grid itself.
inline Blob makeKeyframe(ZstdCodec &z, const Grid &grid,
                         int level = kDefaultZstdLevel) {
  Blob b;
  b.rawLen = uint32_t(grid.size());
  b.crc = crc32(grid);
  if (z.compress(grid.data(), grid.size(), b.comp, level) != CodecStatus::Ok)
    b.comp.clear();
  return b;
}

// Reconstruct a keyframe into out. Fails (out untouched) on decompression error
// or checksum mismatch.
inline bool applyKeyframe(ZstdCodec &z, const Blob &kf, Grid &out) {
  Grid decoded;
  if (z.decompress(kf.comp.data(), kf.comp.size(), kf.rawLen, decoded) !=
      CodecStatus::Ok)
    return false;
  if (crc32(decoded) != kf.crc)
    return false;
  out = std::move(decoded);
  return true;
}

// Diff two grids and compress the XOR. The checksum is of the *current* grid --
// the state the delta reconstructs -- so the receiver can verify the result it
// computed, and therefore that it applied the delta to the right base.
inline Blob makeDelta(ZstdCodec &z, const Grid &base, const Grid &cur,
                      int level = kDefaultZstdLevel) {
  const Grid diff = xorDiff(base, cur);
  Blob b;
  b.rawLen = uint32_t(diff.size());
  b.crc = crc32(cur);
  if (z.compress(diff.data(), diff.size(), b.comp, level) != CodecStatus::Ok)
    b.comp.clear();
  return b;
}

// Apply a delta to dst, which must be the grid the delta was taken against.
// dst is XORed in place, the result is checksummed, and a mismatch is undone
// (XOR is its own inverse) so the replica is left exactly as it was for the
// RESYNC path.
inline bool applyDelta(ZstdCodec &z, Grid &dst, const Blob &delta) {
  Grid diff;
  if (z.decompress(delta.comp.data(), delta.comp.size(), delta.rawLen, diff) !=
      CodecStatus::Ok)
    return false;
  if (!applyXor(dst, diff))
    return false;
  if (crc32(dst) != delta.crc) {
    applyXor(dst, diff); // roll back
    return false;
  }
  return true;
}

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_GRID_CODEC_HPP_
