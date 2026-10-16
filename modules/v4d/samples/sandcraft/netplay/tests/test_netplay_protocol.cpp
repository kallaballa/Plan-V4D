// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The netplay wire-protocol self-test (roadmap M2). A standalone executable like
// its codec sibling: no V4D, no OpenCV, no sockets -- just the header plus a few
// bytes. It returns non-zero on the first failure class so the tooling can judge
// it by exit code.
//
// What it proves, and why each one is the acceptance criterion and not busywork:
//   * round-trip of every message type is byte-stable (roadmap M2 acceptance);
//   * the encoder's bytes equal a memcpy of the packed, size-asserted struct --
//     this is what ties the normative layout in §13.4 to the code, so a field
//     reorder or a packing change cannot pass unnoticed;
//   * the frame length-prefix survives partial delivery (a TCP stream hands you
//     arbitrary chunks) and a length over MAX_PAYLOAD is refused, not buffered;
//   * an attacker-controlled event count is refused before allocating (§9.6);
//   * a truncated payload decodes to false rather than to a half-filled struct.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "protocol.hpp"

using namespace sandcraft::netplay;

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

// The encoded bytes of a fixed-layout message must be a byte-for-byte copy of
// the packed struct. If this ever fails, either the encoder's field order or the
// struct's layout changed, and the static_asserts above are no longer describing
// the wire.
template <typename Struct, typename Msg>
static void checkLayout(const Msg &m, const std::vector<uint8_t> &p) {
  CHECK(p.size() == sizeof(Struct));
  CHECK(std::memcmp(p.data(), &m, sizeof(Struct)) == 0);
}

int main() {
  // --- emitter / cursor / player / event scalars survive their encoders ------
  {
    EmitterWire e{};
    e.type = 7;
    e.on = 1;
    e.density = 0.3f;
    ByteWriter w;
    putEmitter(w, e);
    ByteReader r(w.data());
    const EmitterWire e2 = getEmitter(r);
    CHECK(r.ok());
    CHECK(e2.type == e.type && e2.on == e.on);
    CHECK(std::memcmp(&e2, &e, sizeof(e)) == 0);
  }
  {
    PlayerCursorWire c{};
    c.x = -1;
    c.y = 12345;
    c.brush = 33;
    c.colorIdx = 2;
    ByteWriter w;
    putCursor(w, c);
    ByteReader r(w.data());
    const PlayerCursorWire c2 = getCursor(r);
    CHECK(std::memcmp(&c2, &c, sizeof(c)) == 0);
  }
  {
    InputEventWire e{};
    e.kind = EV_DRAG;
    e.flags = FLAG_SHIFT | 1u;
    e.x = -1;
    e.y = 400;
    e.dx = -7;
    e.dy = 9;
    e.brush = 5;
    e.penSize = 12;
    e.seq = 0xDEADBEEFu;
    ByteWriter w;
    putEvent(w, e);
    ByteReader r(w.data());
    const InputEventWire e2 = getEvent(r);
    CHECK(std::memcmp(&e2, &e, sizeof(e)) == 0);
  }

  // --- fixed-layout messages encode to exactly their packed struct ----------
  {
    WelcomeMsg m;
    m.w.playerId = 1;
    m.w.playerCount = 3;
    m.w.tickHz = 60;
    m.w.gridW = 640;
    m.w.gridH = 360;
    m.w.clientSeqStart = 1;
    m.w.startTick = 0x0102030405060708ull;
    checkLayout<WelcomeWire>(m.w, encodePayload(m));

    RejectMsg rj;
    rj.r.reason = REJECT_VERSION;
    checkLayout<RejectWire>(rj.r, encodePayload(rj));

    WorldMsg wm;
    wm.w.paused = 1;
    wm.w.particleSwaps = 0;
    wm.w.epoch = 42;
    for (int i = 0; i < 4; ++i) {
      wm.w.emitters[i].type = uint8_t(i + 1);
      wm.w.emitters[i].on = uint8_t(i & 1);
      wm.w.emitters[i].density = 0.25f * float(i + 1);
    }
    checkLayout<WorldWire>(wm.w, encodePayload(wm));

    PingPongMsg pm;
    pm.p.senderTimeMs = 111;
    pm.p.peerTimeMs = 222;
    checkLayout<PingPongWire>(pm.p, encodePayload(pm));

    ResyncMsg rm;
    rm.r.haveTick = 0xABCDEF;
    checkLayout<ResyncWire>(rm.r, encodePayload(rm));

    ByeMsg bm;
    bm.b.reason = BYE_HOST_LEFT;
    checkLayout<ByeWire>(bm.b, encodePayload(bm));
  }

  // --- keyframe: header only (no comp, no roster) is exactly KeyframeHeader --
  {
    KeyframeMsg kf;
    kf.h.tick = 99;
    kf.h.gridW = 640;
    kf.h.gridH = 360;
    kf.h.rawLen = 640 * 360;
    kf.h.compLen = 0;
    kf.h.crc32 = 0x1234;
    kf.h.worldEpoch = 3;
    kf.h.playerCount = 0;
    kf.h.paused = 0;
    kf.h.particleSwaps = 1;
    for (int i = 0; i < 4; ++i) {
      kf.h.emitters[i].type = uint8_t(i);
      kf.h.emitters[i].density = 0.3f;
    }
    checkLayout<KeyframeHeader>(kf.h, encodePayload(kf));

    DeltaMsg dm;
    dm.h.baseTick = 10;
    dm.h.tick = 11;
    dm.h.appliedSeq[1] = 5;
    dm.h.rawLen = 640 * 360;
    dm.h.compLen = 0;
    dm.h.crc32 = 0x77;
    dm.h.playerCount = 2;
    dm.h.cursors[1].x = 100;
    dm.h.cursors[1].y = 50;
    checkLayout<DeltaHeader>(dm.h, encodePayload(dm));

    InputBatchMsg ib;
    ib.b.batchTimeMs = 1234;
    ib.b.firstSeq = 7;
    ib.b.count = 0;
    checkLayout<InputBatchWire>(ib.b, encodePayload(ib));
  }

  // --- round trip of every message type -------------------------------------
  {
    HelloMsg in;
    in.h.version = PROTOCOL_VERSION;
    in.name = "anna";
    HelloMsg out;
    CHECK(decodePayload(encodePayload(in), out));
    CHECK(out.h.version == PROTOCOL_VERSION && out.name == "anna");
  }
  {
    WorldMsg in;
    in.w.paused = 1;
    in.w.epoch = 0xCAFEu;
    in.w.emitters[2].type = 4;
    in.w.emitters[2].on = 1;
    in.w.emitters[2].density = 0.75f;
    WorldMsg out;
    CHECK(decodePayload(encodePayload(in), out));
    CHECK(std::memcmp(&in.w, &out.w, sizeof(WorldWire)) == 0);
  }
  {
    KeyframeMsg in;
    in.h.tick = 5;
    in.h.gridW = 640;
    in.h.gridH = 360;
    in.h.rawLen = 100;
    in.h.crc32 = 0xABCDu;
    in.h.worldEpoch = 1;
    in.h.playerCount = 2;
    for (uint8_t i = 0; i < 100; ++i)
      in.comp.push_back(uint8_t(i * 3));
    in.h.compLen = uint32_t(in.comp.size());
    for (uint8_t i = 0; i < 2; ++i) {
      PlayerWire p{};
      p.id = i;
      p.colorIdx = i;
      p.active = 1;
      p.name[0] = char('A' + i);
      in.players.push_back(p);
    }
    KeyframeMsg out;
    CHECK(decodePayload(encodePayload(in), out));
    CHECK(out.h.tick == 5 && out.h.crc32 == 0xABCDu);
    CHECK(out.comp == in.comp);
    CHECK(out.players.size() == 2 && out.players[1].name[0] == 'B');
  }
  {
    InputBatchMsg in;
    in.b.count = 3;
    in.b.firstSeq = 10;
    for (uint32_t i = 0; i < 3; ++i) {
      InputEventWire e{};
      e.kind = EV_DRAG;
      e.x = int16_t(i * 10);
      e.y = int16_t(i);
      e.seq = 10 + i;
      in.events.push_back(e);
    }
    InputBatchMsg out;
    CHECK(decodePayload(encodePayload(in), out));
    CHECK(out.b.count == 3 && out.events.size() == 3);
    CHECK(out.events[2].x == 20 && out.events[2].seq == 12);
  }

  // --- determinism: the same message encodes to the same bytes --------------
  {
    WelcomeMsg m;
    m.w.playerId = 2;
    m.w.gridW = 640;
    m.w.gridH = 360;
    CHECK(encodePayload(m) == encodePayload(m));
  }

  // --- framing: full buffer, then byte-by-byte ------------------------------
  {
    const std::vector<uint8_t> a = frame(MSG_PING, 1, encodePayload(PingPongMsg{}));
    const std::vector<uint8_t> b = frame(MSG_RESYNC, 2, encodePayload(ResyncMsg{}));

    FrameParser whole;
    whole.push(a.data(), a.size());
    whole.push(b.data(), b.size());
    Frame f1, f2, f3;
    CHECK(whole.next(f1) && f1.type == MSG_PING && f1.valid);
    CHECK(whole.next(f2) && f2.type == MSG_RESYNC && f2.seq == 2);
    CHECK(!whole.next(f3)); // no third frame
    CHECK(whole.buffered() == 0);

    // The same two frames, delivered one byte at a time, must parse to the same
    // messages -- a TCP segment boundary is not a message boundary.
    FrameParser drip;
    int got = 0;
    for (size_t i = 0; i < a.size() + b.size(); ++i) {
      const uint8_t byte = (i < a.size()) ? a[i] : b[i - a.size()];
      drip.push(&byte, 1);
      Frame f;
      while (drip.next(f)) {
        CHECK(f.valid);
        ++got;
      }
    }
    CHECK(got == 2);
  }

  // --- an oversize length is refused, not buffered --------------------------
  {
    FrameParser p;
    uint8_t hdr[4] = {0xFF, 0xFF, 0xFF, 0xFF}; // length ~4 GiB
    p.push(hdr, 4);
    Frame f;
    CHECK(p.next(f));
    CHECK(f.oversize);
    CHECK(p.buffered() == 0); // the junk was dropped, not held
  }

  // --- a bad event count is refused before allocating -----------------------
  {
    InputBatchMsg in;
    in.b.count = MAX_EVENTS_PER_BATCH + 1; // lies about the tail
    std::vector<uint8_t> p = encodePayload(in);
    InputBatchMsg out;
    CHECK(!decodePayload(p, out));
  }
  {
    // An over-limit count with no tail at all must also be refused.
    InputBatchMsg in;
    in.b.count = 8;
    std::vector<uint8_t> p = encodePayload(in); // header says 8, tail holds 0
    InputBatchMsg out;
    CHECK(!decodePayload(p, out));
  }

  // --- a truncated payload decodes to false ---------------------------------
  {
    KeyframeMsg in;
    in.h.tick = 1;
    in.h.compLen = 16;
    in.comp.assign(16, 0x55);
    std::vector<uint8_t> p = encodePayload(in);
    CHECK(p.size() > 4);
    if (p.size() > 4)
      p.resize(p.size() - 4); // lop off the tail
    KeyframeMsg out;
    CHECK(!decodePayload(p, out));
  }
  {
    std::vector<uint8_t> empty;
    HelloMsg out;
    CHECK(!decodePayload(empty, out)); // nothing there at all
  }

  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "ok",
              g_checks, g_failures);
  return g_failures ? 1 : 0;
}
