// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The replication engine (specification.md §12): the host half that turns a
// published grid into a keyframe or a chained delta, and the client half that
// turns a keyframe or delta back into a replica.
//
// The single most important rule here is Rule R1 (§12.1): a delta is taken
// against the state that was **last transmitted**, not against the state at the
// previous tick. The host owns that baseline inside HostReplicator. It is what
// makes a skipped tick correct -- if the net thread can only afford to send every
// other frame (Rule R2, frame dropping, explicitly not rate limiting), the delta
// spans `lastSentTick -> tick` and the receiver still reconstructs exactly.
//
// The layers above (a Plan node, or the M2 self-test) only ever see framed bytes
// and an ApplyResult; nothing here touches V4D, OpenCV or a socket.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_REPLICATION_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_REPLICATION_HPP_

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "grid_codec.hpp"
#include "netqueues.hpp"
#include "protocol.hpp"

namespace sandcraft {
namespace netplay {

// A keyframe is forced this often even when deltas are flowing, so any
// undetected divergence is bounded in time (spec §12.3). 300 ticks at ~60 Hz is
// the spec's ~5 s.
inline constexpr uint64_t DEFAULT_KEYFRAME_INTERVAL = 300;

inline uint32_t nowMillis() {
  using namespace std::chrono;
  return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
                      .count() &
                  0xFFFFFFFFu);
}

// --- host side ---------------------------------------------------------------

class HostReplicator {
public:
  explicit HostReplicator(uint64_t keyframeInterval = DEFAULT_KEYFRAME_INTERVAL,
                          int level = kDefaultZstdLevel)
      : keyframeEvery_(keyframeInterval), level_(level) {}

  // Turn the authoritative snapshot into the frame(s) to broadcast for this
  // tick. Returns one framed message: a KEYFRAME when there is no baseline yet,
  // when `forceKeyframe` is set (join/reconnect/resync), or on the periodic
  // interval; otherwise a DELTA against the last transmitted grid.
  //
  // `outExtra` receives any additional framed messages that must accompany this
  // tick (currently none; it exists so a WORLD broadcast can be attached later
  // without changing this signature). Returns the primary frame; an empty vector
  // means "nothing to send".
  std::vector<uint8_t> produce(const GridSnapshot &snap, bool forceKeyframe,
                               std::vector<std::vector<uint8_t>> *outExtra = nullptr) {
    if (snap.cells.empty())
      return {};

    // The periodic keyframe is measured against the last *keyframe*, not the
    // last *transmitted* tick (spec §12.3): `lastSentTick_` advances on every
    // delta, so testing the interval against it would make the expression the
    // span the delta covers (~1 tick) and the periodic keyframe would never
    // fire. A keyframe resets both clocks by definition.
    const bool keyframe = forceKeyframe || !haveBaseline_ ||
                          (snap.tick - lastKeyframeTick_) >= keyframeEvery_;

    std::vector<uint8_t> framed;
    if (keyframe) {
      KeyframeMsg kf;
      kf.h.tick = snap.tick;
      kf.h.gridW = snap.width;
      kf.h.gridH = snap.height;
      kf.h.worldEpoch = snap.world.epoch;
      kf.h.playerCount = uint8_t(snap.roster.size());
      kf.h.paused = snap.world.paused;
      kf.h.particleSwaps = snap.world.particleSwaps;
      for (int i = 0; i < 4; ++i)
        kf.h.emitters[i] = snap.world.emitters[i];
      const Blob b = makeKeyframe(z_, snap.cells, level_);
      kf.h.rawLen = b.rawLen;
      kf.h.crc32 = b.crc;
      kf.comp = b.comp;
      kf.h.compLen = uint32_t(b.comp.size());
      kf.players = snap.roster;
      framed = frame(MSG_KEYFRAME, ++msgSeq_, encodePayload(kf));
    } else {
      DeltaMsg dm;
      dm.h.baseTick = lastSentTick_;
      dm.h.tick = snap.tick;
      for (int i = 0; i < 4; ++i)
        dm.h.appliedSeq[i] = snap.appliedSeq[i];
      dm.h.playerCount = uint8_t(snap.roster.size());
      for (int i = 0; i < 4; ++i)
        dm.h.cursors[i] = snap.cursors[i];
      const Blob b = makeDelta(z_, baseline_, snap.cells, level_);
      dm.h.rawLen = b.rawLen;
      dm.h.crc32 = b.crc;
      dm.comp = b.comp;
      dm.h.compLen = uint32_t(b.comp.size());
      framed = frame(MSG_DELTA, ++msgSeq_, encodePayload(dm));
    }

    if (outExtra)
      (void)outExtra;
    // The baseline advances to exactly what we transmitted, whichever kind it
    // was. This is Rule R1; it is also what makes the skipped-tick case correct.
    baseline_ = snap.cells;
    lastSentTick_ = snap.tick;
    haveBaseline_ = true;
    if (keyframe)
      lastKeyframeTick_ = snap.tick;
    return framed;
  }

  // A joining client gets a WELCOME built here so the wire shape lives in one
  // place with the rest of the catalog (§17.2 step 2).
  std::vector<uint8_t> welcome(uint8_t playerId, uint8_t playerCount,
                               uint16_t gridW, uint16_t gridH, uint64_t startTick) {
    WelcomeMsg w;
    w.w.playerId = playerId;
    w.w.playerCount = playerCount;
    w.w.tickHz = 60;
    w.w.gridW = gridW;
    w.w.gridH = gridH;
    w.w.clientSeqStart = 1;
    w.w.startTick = startTick;
    return frame(MSG_WELCOME, ++msgSeq_, encodePayload(w));
  }

  std::vector<uint8_t> reject(uint8_t reason) {
    RejectMsg r;
    r.r.reason = reason;
    return frame(MSG_REJECT, ++msgSeq_, encodePayload(r));
  }

  // A RESYNC from a client sets this so the next produce() is a keyframe.
  void requestKeyframe() { forceNext_ = true; }
  bool consumeForceNext() {
    const bool f = forceNext_;
    forceNext_ = false;
    return f;
  }

  uint64_t lastSentTick() const { return lastSentTick_; }
  bool hasBaseline() const { return haveBaseline_; }
  uint64_t lastKeyframeTick() const { return lastKeyframeTick_; }

  // Forget the baseline: the next produce() is a keyframe. Used on reconnect.
  void reset() {
    haveBaseline_ = false;
    baseline_.clear();
    lastKeyframeTick_ = 0;
  }

  size_t baselineBytes() const { return baseline_.size(); }

private:
  ZstdCodec z_;
  Grid baseline_;
  uint64_t lastSentTick_ = 0;
  uint64_t lastKeyframeTick_ = 0;
  uint64_t keyframeEvery_;
  int level_;
  bool haveBaseline_ = false;
  bool forceNext_ = false;
  uint32_t msgSeq_ = 0;
};

// --- client side -------------------------------------------------------------

enum class ApplyResult {
  Applied, // a keyframe/delta/world was accepted
  Ignored, // a message that does not change the replica (e.g. PONG)
  Desync,  // baseTick != replica tick, or a checksum failure: needs RESYNC
  Error,   // malformed/undecodable
};

class ClientReplicator {
public:
  // `gridW`/`gridH` come from WELCOME; the replica is sized here so a keyframe
  // and every later delta share one allocation (spec §16.3, copy is the
  // baseline; double-buffering is a backlog item).
  void init(uint16_t gridW, uint16_t gridH, uint8_t myPlayerId) {
    width_ = gridW;
    height_ = gridH;
    myId_ = myPlayerId;
    replica_.assign(size_t(gridW) * gridH, 0);
    haveState_ = false;
  }

  // Apply one framed message to the replica. `replies`, if given, collects
  // framed messages the client must send back (a PONG, a RESYNC).
  ApplyResult apply(const Frame &f, std::vector<std::vector<uint8_t>> *replies = nullptr) {
    switch (f.type) {
    case MSG_KEYFRAME:
      return applyKeyframeMsg(f);
    case MSG_DELTA:
      return applyDeltaMsg(f);
    case MSG_WORLD: {
      WorldMsg w;
      if (!decodePayload(f.payload, w))
        return ApplyResult::Error;
      world_ = w.w;
      return ApplyResult::Applied;
    }
    case MSG_PING: {
      PingPongMsg p;
      if (!decodePayload(f.payload, p))
        return ApplyResult::Error;
      PingPongMsg reply;
      reply.p.senderTimeMs = nowMillis();
      reply.p.peerTimeMs = p.p.senderTimeMs;
      if (replies)
        replies->push_back(frame(MSG_PONG, ++msgSeq_, encodePayload(reply)));
      return ApplyResult::Ignored;
    }
    case MSG_PONG: {
      PingPongMsg p;
      if (!decodePayload(f.payload, p))
        return ApplyResult::Error;
      const uint32_t rtt = nowMillis() - p.p.peerTimeMs;
      lastRttMs_ = rtt;
      return ApplyResult::Ignored;
    }
    case MSG_BYE:
      hostLeft_ = true;
      return ApplyResult::Ignored;
    default:
      return ApplyResult::Ignored;
    }
  }

  // Accessors for the apply node / the self-test.
  const Grid &grid() const { return replica_; }
  uint64_t tick() const { return lastTick_; }
  bool haveState() const { return haveState_; }
  const uint32_t *appliedSeq() const { return appliedSeq_; }
  const WorldWire &world() const { return world_; }
  const PlayerCursorWire *cursors() const { return cursors_; }
  uint8_t playerCount() const { return playerCount_; }
  uint32_t lastRttMs() const { return lastRttMs_; }
  bool hostLeft() const { return hostLeft_; }
  uint32_t desyncCount() const { return desyncs_; }

  // The client half of the handshake (§17.2 step 1).
  std::vector<uint8_t> hello(const std::string &name) {
    HelloMsg h;
    h.h.version = PROTOCOL_VERSION;
    h.h.nameLen = uint8_t(name.size());
    h.name = name;
    return frame(MSG_HELLO, ++msgSeq_, encodePayload(h));
  }

  std::vector<uint8_t> resync() {
    ResyncMsg r;
    r.r.haveTick = lastTick_;
    return frame(MSG_RESYNC, ++msgSeq_, encodePayload(r));
  }

private:
  ApplyResult applyKeyframeMsg(const Frame &f) {
    KeyframeMsg kf;
    if (!decodePayload(f.payload, kf))
      return ApplyResult::Error;
    // The first keyframe sizes the replica (its dimensions are the session's,
    // spec §23); every later one must match, because a session's dimensions are
    // fixed for its lifetime.
    if (width_ == 0 || height_ == 0) {
      width_ = kf.h.gridW;
      height_ = kf.h.gridH;
      replica_.assign(size_t(width_) * size_t(height_), 0);
    } else if (kf.h.gridW != width_ || kf.h.gridH != height_) {
      return ApplyResult::Error;
    }
    Blob b;
    b.comp = kf.comp;
    b.rawLen = kf.h.rawLen;
    b.crc = kf.h.crc32;
    if (!applyKeyframe(z_, b, replica_))
      return ++desyncs_, ApplyResult::Desync;
    lastTick_ = kf.h.tick;
    playerCount_ = kf.h.playerCount;
    roster_ = kf.players;
    world_.paused = kf.h.paused;
    world_.particleSwaps = kf.h.particleSwaps;
    world_.epoch = kf.h.worldEpoch;
    for (int i = 0; i < 4; ++i)
      world_.emitters[i] = kf.h.emitters[i];
    // A keyframe carries no watermark (spec §13.4); the next delta establishes
    // it. Prediction stays empty until then, which is correct.
    haveState_ = true;
    return ApplyResult::Applied;
  }

  ApplyResult applyDeltaMsg(const Frame &f) {
    DeltaMsg dm;
    if (!decodePayload(f.payload, dm))
      return ApplyResult::Error;
    // A delta is applicable iff its base is the replica's tick (§12.1).
    if (dm.h.baseTick != lastTick_) {
      ++desyncs_;
      return ApplyResult::Desync;
    }
    Blob b;
    b.comp = dm.comp;
    b.rawLen = dm.h.rawLen;
    b.crc = dm.h.crc32;
    if (!applyDelta(z_, replica_, b)) {
      ++desyncs_;
      return ApplyResult::Desync;
    }
    lastTick_ = dm.h.tick;
    playerCount_ = dm.h.playerCount;
    for (int i = 0; i < 4; ++i) {
      appliedSeq_[i] = dm.h.appliedSeq[i];
      cursors_[i] = dm.h.cursors[i];
    }
    haveState_ = true;
    return ApplyResult::Applied;
  }

  ZstdCodec z_;
  Grid replica_;
  uint16_t width_ = 0, height_ = 0;
  uint8_t myId_ = 0;
  uint64_t lastTick_ = 0;
  uint32_t appliedSeq_[4] = {0, 0, 0, 0};
  PlayerCursorWire cursors_[4]{};
  WorldWire world_{};
  std::vector<PlayerWire> roster_;
  uint8_t playerCount_ = 0;
  uint32_t lastRttMs_ = 0;
  uint32_t desyncs_ = 0;
  bool haveState_ = false;
  bool hostLeft_ = false;
  uint32_t msgSeq_ = 0;
};

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_REPLICATION_HPP_
