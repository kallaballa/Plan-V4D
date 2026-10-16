// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The Sandcraft netplay wire protocol (specification.md §13).
//
// Every message is length-prefixed (a uint32 payload length, little-endian) and
// the payload opens with an 8-byte MsgHeader. The structures below are the
// normative layout of that payload, declared packed and size-asserted so a wire
// change cannot slip through as a silent ABI drift; the encoder writes them
// field by field through the little-endian ByteWriter rather than memcpy'ing the
// struct, so the bytes are defined by code and not by the compiler's idea of the
// struct. The round-trip test checks the two agree (it asserts the encoded bytes
// equal a memcpy of the packed struct), which is what keeps the asserts honest.
//
// Endianness: every target is little-endian (spec §13.5). The header refuses to
// compile anywhere else rather than growing a byte-swapping encoder nobody would
// test, because a wire that is only correct on the machine that built it is a
// worse outcome than a build error.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_PROTOCOL_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_PROTOCOL_HPP_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "netplay.hpp"

#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "sandcraft netplay: the wire protocol assumes a little-endian host"
#endif

namespace sandcraft {
namespace netplay {

// --- limits (spec §13.6) -----------------------------------------------------
inline constexpr uint32_t MAX_PAYLOAD = 1u << 20; // 1 MiB; larger => close
inline constexpr uint16_t MAX_EVENTS_PER_BATCH = 4096; // host rejects excess
inline constexpr int MAX_NAME = 16;                    // bytes, NUL-padded

// --- message catalog (spec §13.3) --------------------------------------------
enum MsgType : uint8_t {
  MSG_HELLO = 1,
  MSG_WELCOME = 2,
  MSG_REJECT = 3,
  MSG_KEYFRAME = 4,
  MSG_DELTA = 5,
  MSG_INPUT = 6,
  MSG_WORLD = 7,
  MSG_PING = 8,
  MSG_PONG = 9,
  MSG_RESYNC = 10,
  MSG_BYE = 11,
};

// --- input event taxonomy (spec §9.2) ----------------------------------------
enum EventKind : uint8_t {
  EV_MOVE = 0,
  EV_PRESS = 1,
  EV_RELEASE = 2,
  EV_DRAG = 3,
  EV_SCROLL = 4,
  EV_KEY = 5,
  EV_SET_BRUSH = 6,
  EV_SET_PEN = 7,
  EV_COMMAND = 8,
};

// --- command ids (spec §9.5) -------------------------------------------------
enum CommandId : uint8_t {
  CMD_PAUSE_TOGGLE = 1,
  CMD_CLEAR = 2,
  CMD_RANDOMIZE = 3,
  CMD_EMITTER_TOGGLE = 4,    // brush = emitter index
  CMD_EMITTER_DENSITY = 5,   // brush = emitter index, dx = millis of density
  CMD_SWAPS_TOGGLE = 6,
};

enum RejectReason : uint8_t { REJECT_FULL = 1, REJECT_VERSION = 2, REJECT_TOKEN = 3 };
enum ByeReason : uint8_t { BYE_NORMAL = 0, BYE_ERROR = 1, BYE_HOST_LEFT = 2 };

// `flags` bit layout (spec §13.4): bits 0..3 mouse button, bits 4..6 modifiers,
// bit 7 key edge. The helpers keep the shifts in one place.
inline constexpr uint8_t FLAG_BUTTON_MASK = 0x0F;
inline constexpr uint8_t FLAG_SHIFT = 0x10;
inline constexpr uint8_t FLAG_CTRL = 0x20;
inline constexpr uint8_t FLAG_ALT = 0x40;
inline constexpr uint8_t FLAG_KEY_RELEASE = 0x80;

inline uint8_t packModifiers(bool shift, bool ctrl, bool alt) {
  return uint8_t((shift ? FLAG_SHIFT : 0) | (ctrl ? FLAG_CTRL : 0) | (alt ? FLAG_ALT : 0));
}

// --- normative wire structures (spec §13.2, §13.4) ---------------------------
#pragma pack(push, 1)

struct MsgHeader {
  uint8_t type;      // MsgType
  uint8_t version;   // PROTOCOL_VERSION
  uint16_t reserved; // 0
  uint32_t seq;      // sender-local message counter (diagnostics)
};

struct HelloWire {
  uint8_t version;
  uint8_t nameLen;
  uint8_t reserved[2];
  // name[nameLen] follows in the payload
};

struct WelcomeWire {
  uint8_t playerId;   // 0 == host
  uint8_t playerCount;
  uint8_t tickHz;
  uint8_t reserved;
  uint16_t gridW, gridH;
  uint32_t clientSeqStart;
  uint64_t startTick;
};

struct RejectWire { uint8_t reason; };

struct EmitterWire {
  uint8_t type;
  uint8_t on;
  float density;
  uint8_t pad[2];
};

struct PlayerWire {
  uint8_t id, colorIdx, active, reserved;
  char name[MAX_NAME];
};

struct PlayerCursorWire {
  int16_t x, y;
  uint8_t brush, colorIdx;
};

struct KeyframeHeader {
  uint64_t tick;
  uint16_t gridW, gridH;
  uint32_t rawLen; // == gridW*gridH
  uint32_t compLen; // zstd bytes that follow the header
  uint32_t crc32;   // of the raw grid
  uint32_t worldEpoch;
  uint8_t playerCount, paused, particleSwaps, reserved;
  EmitterWire emitters[4];
  // followed by: compLen bytes, then playerCount * PlayerWire
};

struct DeltaHeader {
  uint64_t baseTick, tick;
  uint32_t appliedSeq[4];
  uint32_t rawLen, compLen, crc32;
  uint8_t playerCount, reserved[3];
  PlayerCursorWire cursors[4];
  // followed by: compLen bytes (zstd of the XOR)
};

struct InputEventWire {
  uint8_t kind;    // EventKind
  uint8_t flags;   // buttons | modifiers | key edge
  int16_t x, y;    // cell coords, or -1
  int16_t dx, dy;  // scroll / drag delta
  uint8_t brush;   // ParticleType (context-dependent)
  uint8_t penSize; // 1..32
  uint32_t seq;    // per-client monotonic
};

struct InputBatchWire {
  uint32_t batchTimeMs;
  uint32_t firstSeq;
  uint16_t count, reserved;
  // followed by: count * InputEventWire
};

struct WorldWire {
  uint8_t paused, particleSwaps, reserved[2];
  uint32_t epoch;
  EmitterWire emitters[4];
};

struct PingPongWire { uint32_t senderTimeMs, peerTimeMs; };
struct ResyncWire { uint64_t haveTick; };
struct ByeWire { uint8_t reason; };

#pragma pack(pop)

static_assert(sizeof(MsgHeader) == 8, "MsgHeader must be 8 bytes");
static_assert(sizeof(HelloWire) == 4, "HelloWire must be 4 bytes");
static_assert(sizeof(WelcomeWire) == 20, "WelcomeWire must be 20 bytes");
static_assert(sizeof(RejectWire) == 1, "RejectWire must be 1 byte");
static_assert(sizeof(EmitterWire) == 8, "EmitterWire must be 8 bytes");
static_assert(sizeof(PlayerWire) == 20, "PlayerWire must be 20 bytes");
static_assert(sizeof(PlayerCursorWire) == 6, "PlayerCursorWire must be 6 bytes");
static_assert(sizeof(KeyframeHeader) == 64, "KeyframeHeader must be 64 bytes");
static_assert(sizeof(DeltaHeader) == 72, "DeltaHeader must be 72 bytes");
static_assert(sizeof(InputEventWire) == 16, "InputEventWire must be 16 bytes");
static_assert(sizeof(InputBatchWire) == 12, "InputBatchWire must be 12 bytes");
static_assert(sizeof(WorldWire) == 40, "WorldWire must be 40 bytes");
static_assert(sizeof(PingPongWire) == 8, "PingPongWire must be 8 bytes");
static_assert(sizeof(ResyncWire) == 8, "ResyncWire must be 8 bytes");
static_assert(sizeof(ByeWire) == 1, "ByeWire must be 1 byte");

// --- little-endian byte writer / reader --------------------------------------
//
// The reader is fail-safe: once a read runs off the end it stays failed, every
// later read returns its zero value, and the caller checks ok() once. That is
// what keeps a truncated or hostile frame from turning into a wild index -- the
// alternative, returning partial values, is how a decoder corrupts state.
class ByteWriter {
public:
  void u8(uint8_t v) { buf_.push_back(v); }
  void u16(uint16_t v) {
    buf_.push_back(uint8_t(v));
    buf_.push_back(uint8_t(v >> 8));
  }
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i)
      buf_.push_back(uint8_t(v >> (8 * i)));
  }
  void u64(uint64_t v) {
    for (int i = 0; i < 8; ++i)
      buf_.push_back(uint8_t(v >> (8 * i)));
  }
  void i16(int16_t v) { u16(uint16_t(v)); }
  void f32(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    u32(bits);
  }
  void bytes(const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    buf_.insert(buf_.end(), b, b + n);
  }
  void blob(const std::vector<uint8_t> &v) { bytes(v.data(), v.size()); }
  // A fixed-width, NUL-padded name: exactly MAX_NAME bytes, never a terminator
  // beyond the field, so the reader can copy it without a scan.
  void name16(const std::string &s) {
    for (int i = 0; i < MAX_NAME; ++i)
      buf_.push_back(i < int(s.size()) ? uint8_t(s[size_t(i)]) : 0);
  }
  size_t size() const { return buf_.size(); }
  std::vector<uint8_t> &data() { return buf_; }
  const std::vector<uint8_t> &data() const { return buf_; }
  std::vector<uint8_t> take() { return std::move(buf_); }

private:
  std::vector<uint8_t> buf_;
};

class ByteReader {
public:
  ByteReader(const uint8_t *p, size_t n) : p_(p), n_(n) {}
  explicit ByteReader(const std::vector<uint8_t> &v) : p_(v.data()), n_(v.size()) {}

  bool ok() const { return ok_; }
  size_t remaining() const { return ok_ ? (n_ - off_) : 0; }

  uint8_t u8() {
    if (!have(1))
      return 0;
    return p_[off_++];
  }
  uint16_t u16() {
    if (!have(2))
      return 0;
    const uint16_t v = uint16_t(p_[off_]) | uint16_t(p_[off_ + 1]) << 8;
    off_ += 2;
    return v;
  }
  uint32_t u32() {
    if (!have(4))
      return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= uint32_t(p_[off_ + i]) << (8 * i);
    off_ += 4;
    return v;
  }
  uint64_t u64() {
    if (!have(8))
      return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= uint64_t(p_[off_ + i]) << (8 * i);
    off_ += 8;
    return v;
  }
  int16_t i16() { return int16_t(u16()); }
  float f32() {
    const uint32_t bits = u32();
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  std::string name16() {
    char tmp[MAX_NAME];
    if (!bytes(tmp, MAX_NAME))
      return {};
    size_t n = 0;
    while (n < MAX_NAME && tmp[n])
      ++n;
    return std::string(tmp, n);
  }
  bool bytes(void *dst, size_t n) {
    if (!have(n))
      return false;
    std::memcpy(dst, p_ + off_, n);
    off_ += n;
    return true;
  }
  bool blob(std::vector<uint8_t> &dst, size_t n) {
    if (!have(n))
      return false;
    dst.assign(p_ + off_, p_ + off_ + n);
    off_ += n;
    return true;
  }

private:
  bool have(size_t n) {
    if (!ok_ || off_ + n > n_) {
      ok_ = false;
      return false;
    }
    return true;
  }
  const uint8_t *p_;
  size_t n_;
  size_t off_ = 0;
  bool ok_ = true;
};

// --- message bodies ----------------------------------------------------------
//
// The fixed-layout structs above are embedded by value so there is one
// declaration of every field; only the variable-length tails (names, the
// compressed grid, the event list, the roster) live beside them.

struct HelloMsg {
  HelloWire h{};
  std::string name;
};

struct WelcomeMsg { WelcomeWire w{}; };
struct RejectMsg { RejectWire r{}; };

struct KeyframeMsg {
  KeyframeHeader h{};
  std::vector<uint8_t> comp;      // zstd of the settled grid
  std::vector<PlayerWire> players;
};

struct DeltaMsg {
  DeltaHeader h{};
  std::vector<uint8_t> comp;      // zstd of the XOR diff
};

struct InputBatchMsg {
  InputBatchWire b{};
  std::vector<InputEventWire> events;
};

struct WorldMsg { WorldWire w{}; };
struct PingPongMsg { PingPongWire p{}; };
struct ResyncMsg { ResyncWire r{}; };
struct ByeMsg { ByeWire b{}; };

// --- encoders / decoders -----------------------------------------------------
//
// Each returns the payload (everything after MsgHeader). The caller frames it
// with frame(). The decoders take a ByteReader positioned at the payload start
// and return false on any structural failure (short buffer, count over a limit),
// leaving the message untouched.

inline void putEmitter(ByteWriter &w, const EmitterWire &e) {
  w.u8(e.type);
  w.u8(e.on);
  w.f32(e.density);
  w.u8(0);
  w.u8(0);
}

inline EmitterWire getEmitter(ByteReader &r) {
  EmitterWire e{};
  e.type = r.u8();
  e.on = r.u8();
  e.density = r.f32();
  r.u8();
  r.u8();
  return e;
}

inline void putCursor(ByteWriter &w, const PlayerCursorWire &c) {
  w.i16(c.x);
  w.i16(c.y);
  w.u8(c.brush);
  w.u8(c.colorIdx);
}

inline PlayerCursorWire getCursor(ByteReader &r) {
  PlayerCursorWire c{};
  c.x = r.i16();
  c.y = r.i16();
  c.brush = r.u8();
  c.colorIdx = r.u8();
  return c;
}

inline void putPlayer(ByteWriter &w, const PlayerWire &p) {
  w.u8(p.id);
  w.u8(p.colorIdx);
  w.u8(p.active);
  w.u8(p.reserved);
  w.bytes(p.name, MAX_NAME);
}

inline PlayerWire getPlayer(ByteReader &r) {
  PlayerWire p{};
  p.id = r.u8();
  p.colorIdx = r.u8();
  p.active = r.u8();
  p.reserved = r.u8();
  r.bytes(p.name, MAX_NAME);
  return p;
}

inline void putEvent(ByteWriter &w, const InputEventWire &e) {
  w.u8(e.kind);
  w.u8(e.flags);
  w.i16(e.x);
  w.i16(e.y);
  w.i16(e.dx);
  w.i16(e.dy);
  w.u8(e.brush);
  w.u8(e.penSize);
  w.u32(e.seq);
}

inline InputEventWire getEvent(ByteReader &r) {
  InputEventWire e{};
  e.kind = r.u8();
  e.flags = r.u8();
  e.x = r.i16();
  e.y = r.i16();
  e.dx = r.i16();
  e.dy = r.i16();
  e.brush = r.u8();
  e.penSize = r.u8();
  e.seq = r.u32();
  return e;
}

inline std::vector<uint8_t> encodePayload(const HelloMsg &m) {
  ByteWriter w;
  w.u8(m.h.version);
  w.u8(uint8_t(m.name.size()));
  w.u8(0);
  w.u8(0);
  w.bytes(m.name.data(), m.name.size());
  return w.take();
}

inline bool decodePayload(const std::vector<uint8_t> &p, HelloMsg &m) {
  ByteReader r(p);
  m.h.version = r.u8();
  m.h.nameLen = r.u8();
  r.u8();
  r.u8();
  if (!r.ok() || m.h.nameLen > r.remaining())
    return false;
  m.name.assign(reinterpret_cast<const char *>(p.data()) + 4, m.h.nameLen);
  return true;
}

inline std::vector<uint8_t> encodePayload(const WelcomeMsg &m) {
  ByteWriter w;
  w.u8(m.w.playerId);
  w.u8(m.w.playerCount);
  w.u8(m.w.tickHz);
  w.u8(m.w.reserved);
  w.u16(m.w.gridW);
  w.u16(m.w.gridH);
  w.u32(m.w.clientSeqStart);
  w.u64(m.w.startTick);
  return w.take();
}

inline bool decodePayload(const std::vector<uint8_t> &p, WelcomeMsg &m) {
  ByteReader r(p);
  m.w.playerId = r.u8();
  m.w.playerCount = r.u8();
  m.w.tickHz = r.u8();
  m.w.reserved = r.u8();
  m.w.gridW = r.u16();
  m.w.gridH = r.u16();
  m.w.clientSeqStart = r.u32();
  m.w.startTick = r.u64();
  return r.ok();
}

inline std::vector<uint8_t> encodePayload(const RejectMsg &m) {
  ByteWriter w;
  w.u8(m.r.reason);
  return w.take();
}

inline bool decodePayload(const std::vector<uint8_t> &p, RejectMsg &m) {
  ByteReader r(p);
  m.r.reason = r.u8();
  return r.ok();
}

inline std::vector<uint8_t> encodePayload(const KeyframeMsg &m) {
  ByteWriter w;
  w.u64(m.h.tick);
  w.u16(m.h.gridW);
  w.u16(m.h.gridH);
  w.u32(m.h.rawLen);
  w.u32(m.h.compLen);
  w.u32(m.h.crc32);
  w.u32(m.h.worldEpoch);
  w.u8(m.h.playerCount);
  w.u8(m.h.paused);
  w.u8(m.h.particleSwaps);
  w.u8(m.h.reserved);
  for (const EmitterWire &e : m.h.emitters)
    putEmitter(w, e);
  w.blob(m.comp);
  for (const PlayerWire &pl : m.players)
    putPlayer(w, pl);
  return w.take();
}

inline bool decodePayload(const std::vector<uint8_t> &p, KeyframeMsg &m) {
  ByteReader r(p);
  m.h.tick = r.u64();
  m.h.gridW = r.u16();
  m.h.gridH = r.u16();
  m.h.rawLen = r.u32();
  m.h.compLen = r.u32();
  m.h.crc32 = r.u32();
  m.h.worldEpoch = r.u32();
  m.h.playerCount = r.u8();
  m.h.paused = r.u8();
  m.h.particleSwaps = r.u8();
  m.h.reserved = r.u8();
  for (EmitterWire &e : m.h.emitters)
    e = getEmitter(r);
  if (!r.ok())
    return false;
  if (!r.blob(m.comp, m.h.compLen))
    return false;
  if (m.h.playerCount > 4)
    return false;
  m.players.clear();
  for (uint8_t i = 0; i < m.h.playerCount; ++i)
    m.players.push_back(getPlayer(r));
  return r.ok();
}

inline std::vector<uint8_t> encodePayload(const DeltaMsg &m) {
  ByteWriter w;
  w.u64(m.h.baseTick);
  w.u64(m.h.tick);
  for (uint32_t s : m.h.appliedSeq)
    w.u32(s);
  w.u32(m.h.rawLen);
  w.u32(m.h.compLen);
  w.u32(m.h.crc32);
  w.u8(m.h.playerCount);
  w.u8(0);
  w.u8(0);
  w.u8(0);
  for (const PlayerCursorWire &c : m.h.cursors)
    putCursor(w, c);
  w.blob(m.comp);
  return w.take();
}

inline bool decodePayload(const std::vector<uint8_t> &p, DeltaMsg &m) {
  ByteReader r(p);
  m.h.baseTick = r.u64();
  m.h.tick = r.u64();
  for (uint32_t &s : m.h.appliedSeq)
    s = r.u32();
  m.h.rawLen = r.u32();
  m.h.compLen = r.u32();
  m.h.crc32 = r.u32();
  m.h.playerCount = r.u8();
  r.u8();
  r.u8();
  r.u8();
  for (PlayerCursorWire &c : m.h.cursors)
    c = getCursor(r);
  if (!r.ok())
    return false;
  return r.blob(m.comp, m.h.compLen);
}

inline std::vector<uint8_t> encodePayload(const InputBatchMsg &m) {
  ByteWriter w;
  w.u32(m.b.batchTimeMs);
  w.u32(m.b.firstSeq);
  w.u16(m.b.count);
  w.u16(m.b.reserved);
  for (const InputEventWire &e : m.events)
    putEvent(w, e);
  return w.take();
}

inline bool decodePayload(const std::vector<uint8_t> &p, InputBatchMsg &m) {
  ByteReader r(p);
  m.b.batchTimeMs = r.u32();
  m.b.firstSeq = r.u32();
  m.b.count = r.u16();
  m.b.reserved = r.u16();
  if (!r.ok())
    return false;
  // Reject before allocating: the count is attacker-controlled (spec §9.6).
  if (m.b.count > MAX_EVENTS_PER_BATCH)
    return false;
  // And the payload must actually be able to hold them.
  if (size_t(m.b.count) * sizeof(InputEventWire) > r.remaining())
    return false;
  m.events.clear();
  m.events.reserve(m.b.count);
  for (uint16_t i = 0; i < m.b.count; ++i)
    m.events.push_back(getEvent(r));
  return r.ok();
}

inline std::vector<uint8_t> encodePayload(const WorldMsg &m) {
  ByteWriter w;
  w.u8(m.w.paused);
  w.u8(m.w.particleSwaps);
  w.u8(0);
  w.u8(0);
  w.u32(m.w.epoch);
  for (const EmitterWire &e : m.w.emitters)
    putEmitter(w, e);
  return w.take();
}

inline bool decodePayload(const std::vector<uint8_t> &p, WorldMsg &m) {
  ByteReader r(p);
  m.w.paused = r.u8();
  m.w.particleSwaps = r.u8();
  r.u8();
  r.u8();
  m.w.epoch = r.u32();
  for (EmitterWire &e : m.w.emitters)
    e = getEmitter(r);
  return r.ok();
}

inline std::vector<uint8_t> encodePayload(const PingPongMsg &m) {
  ByteWriter w;
  w.u32(m.p.senderTimeMs);
  w.u32(m.p.peerTimeMs);
  return w.take();
}

inline bool decodePayload(const std::vector<uint8_t> &p, PingPongMsg &m) {
  ByteReader r(p);
  m.p.senderTimeMs = r.u32();
  m.p.peerTimeMs = r.u32();
  return r.ok();
}

inline std::vector<uint8_t> encodePayload(const ResyncMsg &m) {
  ByteWriter w;
  w.u64(m.r.haveTick);
  return w.take();
}

inline bool decodePayload(const std::vector<uint8_t> &p, ResyncMsg &m) {
  ByteReader r(p);
  m.r.haveTick = r.u64();
  return r.ok();
}

inline std::vector<uint8_t> encodePayload(const ByeMsg &m) {
  ByteWriter w;
  w.u8(m.b.reason);
  return w.take();
}

inline bool decodePayload(const std::vector<uint8_t> &p, ByeMsg &m) {
  ByteReader r(p);
  m.b.reason = r.u8();
  return r.ok();
}

// --- framing (spec §13.1) ----------------------------------------------------
//
// A datagram on the wire is: uint32 length | MsgHeader | payload. frame() builds
// it from a type, the sender's message counter and the encoded payload.

inline std::vector<uint8_t> frame(MsgType type, uint32_t seq,
                                  const std::vector<uint8_t> &payload) {
  ByteWriter w;
  const uint32_t len = uint32_t(sizeof(MsgHeader) + payload.size());
  w.u32(len);
  w.u8(uint8_t(type));
  w.u8(PROTOCOL_VERSION);
  w.u16(0);
  w.u32(seq);
  w.blob(payload);
  return w.take();
}

struct Frame {
  MsgType type = MSG_HELLO;
  uint8_t version = 0;
  uint32_t seq = 0;
  std::vector<uint8_t> payload;
  bool valid = false;
  bool oversize = false; // length > MAX_PAYLOAD: caller should close
};

// Consume bytes from a reassembly buffer and pop complete frames. A byte stream
// delivers partial messages, so the parser keeps what it cannot yet complete.
// It also rejects a length over MAX_PAYLOAD rather than trying to buffer it.
class FrameParser {
public:
  void push(const uint8_t *p, size_t n) { buf_.insert(buf_.end(), p, p + n); }

  bool next(Frame &out) {
    if (buf_.size() < 4)
      return false;
    const uint32_t len = uint32_t(buf_[0]) | uint32_t(buf_[1]) << 8 |
                         uint32_t(buf_[2]) << 16 | uint32_t(buf_[3]) << 24;
    if (len > MAX_PAYLOAD) {
      out = Frame{};
      out.oversize = true;
      buf_.clear();
      return true;
    }
    if (buf_.size() < 4 + size_t(len))
      return false;
    out = Frame{};
    out.type = MsgType(buf_[4]);
    out.version = buf_[5];
    out.seq = uint32_t(buf_[8]) | uint32_t(buf_[9]) << 8 | uint32_t(buf_[10]) << 16 |
              uint32_t(buf_[11]) << 24;
    out.payload.assign(buf_.begin() + 12, buf_.begin() + 4 + len);
    out.valid = (len >= sizeof(MsgHeader));
    buf_.erase(buf_.begin(), buf_.begin() + 4 + len);
    return true;
  }

  size_t buffered() const { return buf_.size(); }

private:
  std::vector<uint8_t> buf_;
};

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_PROTOCOL_HPP_
