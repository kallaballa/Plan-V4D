// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The zstd half of the netplay delta codec (specification.md §14.3).
//
// A thin RAII wrapper around one ZSTD_CCtx and one ZSTD_DCtx. Reusing the two
// contexts across frames matters: zstd allocates its tables and match state in
// the context, and a fresh context per frame spends more on setup than on the
// 230 KB it is asked to (de)compress. The class is move-less and copy-less --
// two contexts, one owner -- and is meant to be held for the process lifetime
// by the net thread (the host compresses, a client decompresses; either side
// may hold a codec and use one direction).
//
// The header is inline-only on purpose: the sample is one translation unit and
// so is the codec test, and the wrapper has no state beyond the two opaque
// pointers, so there is nothing to put in a .cpp. It is unit-testable without
// sockets: compress() and decompress() are pure with respect to all but the
// context's own scratch.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_ZSTD_CODEC_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_ZSTD_CODEC_HPP_

#include <cstddef>
#include <cstdint>
#include <vector>

#include <zstd.h>

namespace sandcraft {
namespace netplay {

// Host favour-ratio default (spec §12.4); the fallback is level 1, and the
// roadmap measures whether 3 is worth the CPU before changing it.
inline constexpr int kDefaultZstdLevel = 3;

enum class CodecStatus {
  Ok,
  Error, // zstd reported an error; its code is stderr-only diagnostics
};

// One pair of zstd contexts, reused for every frame.
class ZstdCodec {
public:
  ZstdCodec() {
    cctx_ = ZSTD_createCCtx();
    dctx_ = ZSTD_createDCtx();
  }
  ~ZstdCodec() {
    if (cctx_)
      ZSTD_freeCCtx(cctx_);
    if (dctx_)
      ZSTD_freeDCtx(dctx_);
  }
  ZstdCodec(const ZstdCodec &) = delete;
  ZstdCodec &operator=(const ZstdCodec &) = delete;

  bool ok() const { return cctx_ != nullptr && dctx_ != nullptr; }

  // Upper bound on the compressed size of srcLen bytes; the destination is
  // sized to this once and then reused by resizing down to the actual result.
  static size_t compressBound(size_t srcLen) {
    return ZSTD_compressBound(srcLen);
  }

  // Compress src into dst, resizing dst to the exact compressed length. Empty
  // input is valid and compresses to a small frame; dst is always overwritten.
  CodecStatus compress(const uint8_t *src, size_t srcLen,
                       std::vector<uint8_t> &dst,
                       int level = kDefaultZstdLevel) {
    if (!cctx_)
      return CodecStatus::Error;
    dst.resize(compressBound(srcLen));
    // A zero-length source is legal for zstd; it still has a src pointer, so
    // give it a valid one rather than null.
    static const uint8_t empty = 0;
    const size_t written =
        ZSTD_compressCCtx(cctx_, dst.data(), dst.size(), srcLen ? src : &empty,
                          srcLen, level);
    if (ZSTD_isError(written))
      return CodecStatus::Error;
    dst.resize(written);
    return CodecStatus::Ok;
  }

  // Decompress src into exactly rawLen bytes. rawLen is known to both sides
  // (the grid is W*H), so no size framing is needed in the payload (spec §12.4).
  // dst is resized to rawLen; on any error dst is left empty.
  CodecStatus decompress(const uint8_t *src, size_t srcLen, size_t rawLen,
                         std::vector<uint8_t> &dst) {
    if (!dctx_)
      return CodecStatus::Error;
    dst.resize(rawLen);
    const size_t written =
        ZSTD_decompressDCtx(dctx_, dst.data(), dst.size(), src, srcLen);
    if (ZSTD_isError(written) || written != rawLen) {
      dst.clear();
      return CodecStatus::Error;
    }
    return CodecStatus::Ok;
  }

private:
  ZSTD_CCtx *cctx_ = nullptr;
  ZSTD_DCtx *dctx_ = nullptr;
};

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_ZSTD_CODEC_HPP_
