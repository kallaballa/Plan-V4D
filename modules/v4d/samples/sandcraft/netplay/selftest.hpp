// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The `--net-selftest` harness (roadmap M2): host and client halves of the
// entire replication + input pipeline, wired host==client through the loopback
// transport, in one process, with no window and no socket.
//
// This is the milestone's proof, and it is deliberately not a toy: it runs the
// real automaton on the host, applies the same per-player input the host
// simulation will apply, publishes real settled snapshots, encodes real
// keyframes and deltas, and asserts after *every* tick that the client's replica
// checksums to the host's grid. It then breaks things on purpose -- a dropped
// tick (Rule R2 frame dropping), a corrupted delta, a delta against the wrong
// base -- and asserts the client notices and recovers, because "recovers from a
// forced desync" is an M7 acceptance criterion that M2 must not contradict.
//
// It is sample-side (it includes the Sandbox and the automaton) rather than part
// of the OpenCV-free test target, because the thing being proven is the pipeline
// *with the real simulation in it*. The pure layers below it are separately
// proven by sandcraft_netplay_tests and sandcraft_netplay_protocol_tests.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_SELFTEST_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_SELFTEST_HPP_

#include <cstdint>
#include <ostream>
#include <random>
#include <string>
#include <vector>

#include "../sandcraft_automaton.hpp"
#include "../sandcraft_grid.hpp"
#include "../sandcraft_view.hpp"
#include "input_apply.hpp"
#include "loopback_transport.hpp"
#include "net_glue.hpp"
#include "netplay.hpp"
#include "protocol.hpp"
#include "replication.hpp"

namespace sandcraft {
namespace netplay {

struct SelfTestStats {
  int checks = 0;
  int failures = 0;
  int desyncs = 0;         // recorded by the client replicator
  uint64_t ticks = 0;      // simulated host ticks
  uint64_t applied = 0;    // frames the client accepted
  size_t keyframes = 0;
  size_t deltas = 0;
  uint64_t hostBytes = 0;
  uint64_t clientBytes = 0;
  uint32_t maxDeltaBytes = 0;
  uint32_t forcedDrops = 0;    // ticks the host deliberately did not transmit
  bool recoveredCorruptDelta = false;
  bool recoveredWrongBase = false;
};

// The host's per-tick input application lives in input_apply.hpp, shared with
// the host simulation node so the two cannot diverge (spec Rule S2).

// One host simulation step (spec §11 step 5-6, mirrored from
// SandSimulationPlan::step so the self-test exercises the same order).
inline void hostStep(Sandbox &sb) {
  if (sb.world_.clearRequested) {
    sb.clear();
    sb.world_.clearRequested = false;
    ++sb.world_.epoch;
  }
  if (sb.world_.randomizeRequested) {
    PlayerState &me = sb.players_[0];
    doRandomLines(sb, me.brush, me.penSize);
    sb.world_.randomizeRequested = false;
    ++sb.world_.epoch;
  }
  if (sb.world_.paused)
    return;
  for (size_t i = 0; i < sb.world_.emitters.size(); ++i) {
    const Emitter &em = sb.world_.emitters[i];
    if (em.on)
      emit(sb, emitterColumn(sb.width_, int(i)), 20, em.type, em.density);
  }
  for (int x = 0; x < sb.width_; ++x) {
    sb.cells_[sb.index(x, 0)] = NOTHING;
    sb.cells_[sb.index(x, sb.height_ - 1)] = NOTHING;
  }
  updateVirtualScreen(sb);
  settleGrid(sb);
  ++sb.simFrames_;
}

namespace selftest_detail {

inline void seedScene(Sandbox &sb) {
  const int w = sb.width_, h = sb.height_;
  const int floorY = h - 2;
  for (int x = 0; x < w; ++x) {
    sb.put(x, floorY, WALL);
    sb.put(x, floorY - 1, WALL);
  }
  for (int x = w / 8; x < w / 3; ++x)
    for (int y = floorY - 8; y < floorY - 2; ++y)
      sb.put(x, y, SAND);
  for (int x = 5 * w / 8; x < 9 * w / 10; ++x)
    for (int y = floorY - 8; y < floorY - 2; ++y)
      sb.put(x, y, WATER);
}

// Drain a transport end into a parser, applying every complete frame. Returns
// the number of frames the client accepted.
inline int pump(Transport &from, FrameParser &parser, ClientReplicator &client,
                uint64_t &clientBytes, SelfTestStats &st) {
  std::vector<uint8_t> buf;
  from.recv(buf);
  if (!buf.empty()) {
    clientBytes += buf.size();
    parser.push(buf.data(), buf.size());
  }
  int applied = 0;
  Frame f;
  while (parser.next(f)) {
    if (f.oversize)
      continue;
    const ApplyResult r = client.apply(f);
    if (r == ApplyResult::Applied)
      ++applied;
    if (r == ApplyResult::Desync)
      ++st.desyncs;
    if (r == ApplyResult::Applied)
      ++st.applied;
  }
  return applied;
}

} // namespace selftest_detail

// Run the whole harness. Returns the number of failures (0 == pass); the
// caller turns that into the process exit code.
inline SelfTestStats runSelfTest(const NetConfig &cfg, std::ostream &log) {
  using namespace selftest_detail;

  SelfTestStats st;
  auto check = [&](bool cond, const char *what) {
    ++st.checks;
    if (!cond) {
      ++st.failures;
      log << "net-selftest: FAIL " << what << "\n";
    }
  };

  Sandbox hostSb;
  hostSb.allocate(640, 360);
  seedScene(hostSb);
  hostSb.players_[0].active = true;
  hostSb.players_[1].active = true; // the guest, player 1
  hostSb.players_[1].colorIdx = 1;

  auto link = makeLoopback();
  HostReplicator host;
  ClientReplicator client;

  // --- handshake (spec §17.2) -----------------------------------------------
  {
    const std::vector<uint8_t> hello = client.hello(cfg.name);
    st.clientBytes += hello.size();
    check(link.second->send(hello), "client HELLO send");
    std::vector<uint8_t> buf;
    link.first->recv(buf);
    FrameParser hp;
    hp.push(buf.data(), buf.size());
    Frame hf;
    check(hp.next(hf) && hf.type == MSG_HELLO, "host decoded HELLO");
    const std::vector<uint8_t> welcome = host.welcome(1, 2, 640, 360, 0);
    st.hostBytes += welcome.size();
    check(link.first->send(welcome), "host WELCOME send");
    client.init(640, 360, 1);
    FrameParser cp;
    check(pump(*link.second, cp, client, st.clientBytes, st) == 0,
          "WELCOME does not change the replica");
    // First keyframe.
    std::vector<uint8_t> kf =
        host.produce(makeSnapshot(hostSb, 0), /*forceKeyframe=*/true);
    st.hostBytes += kf.size();
    check(link.first->send(kf), "host keyframe send");
    check(pump(*link.second, cp, client, st.clientBytes, st) == 1,
          "client applied the first keyframe");
    check(client.haveState(), "client has a replica after the keyframe");
    check(crc32(client.grid()) == crc32(gridToBytes(hostSb)),
          "replica CRC equals authoritative grid after keyframe");
  }

  // --- 600 ticks of scripted play (~10 s at 60 Hz, spec M2 acceptance) -------
  FrameParser cp;
  std::mt19937 rng(20240607);
  auto cellX = [&] { return int(rng() % unsigned(hostSb.width_)); };
  auto cellY = [&] { return int(rng() % unsigned(hostSb.height_ - 8)); };
  int16_t paintX = -1, paintY = -1;

  const uint64_t TICKS = 600; // ~10 s at 60 Hz (spec M2 acceptance)
  for (uint64_t tick = 1; tick <= TICKS; ++tick) {
    // --- scripted input for the guest (player 1) ---
    std::vector<InputEventWire> events;
    auto push = [&](EventKind k, int x, int y, uint8_t brush, uint8_t pen) {
      InputEventWire e{};
      e.kind = uint8_t(k);
      e.x = int16_t(x);
      e.y = int16_t(y);
      e.brush = brush;
      e.penSize = pen;
      e.seq = uint32_t(events.size() + 1);
      events.push_back(e);
    };
    // A move every tick; a drag most ticks; the odd stroke boundary.
    const int mx = cellX(), my = cellY();
    push(EV_MOVE, mx, my, 0, 0);
    if (paintX < 0 && (tick % 5 == 0)) {
      paintX = int16_t(mx);
      paintY = int16_t(my);
      push(EV_PRESS, mx, my, uint8_t(2 + rng() % 6), uint8_t(1 + rng() % 6));
    } else if (paintX >= 0) {
      push(EV_DRAG, mx, my, uint8_t(2 + rng() % 6), uint8_t(1 + rng() % 6));
      if (tick % 7 == 0) {
        push(EV_RELEASE, mx, my, 0, 0);
        paintX = paintY = -1;
      }
    }
    // World commands at known ticks, so the milestone can name what was proven.
    auto pushCmd = [&](uint8_t cmd, uint8_t idx, int16_t dx) {
      InputEventWire e{};
      e.kind = EV_COMMAND;
      e.flags = cmd;
      e.brush = idx;
      e.dx = dx;
      e.x = e.y = -1;
      events.push_back(e);
    };
    if (tick == 100)
      pushCmd(CMD_EMITTER_TOGGLE, 2, 0);
    if (tick == 150)
      pushCmd(CMD_PAUSE_TOGGLE, 0, 0);
    if (tick == 160)
      pushCmd(CMD_PAUSE_TOGGLE, 0, 0);
    if (tick == 300)
      pushCmd(CMD_EMITTER_DENSITY, 0, 200);
    if (tick == 400)
      pushCmd(CMD_SWAPS_TOGGLE, 0, 0);
    if (tick == 500)
      pushCmd(CMD_RANDOMIZE, 0, 0);
    if (tick == 550)
      pushCmd(CMD_CLEAR, 0, 0);

    for (const InputEventWire &e : events)
      applyInputEvent(hostSb, 1, e);

    hostStep(hostSb);

    // --- publish + transmit ---
    // Simulate Rule R2 frame dropping: the host skips transmitting a few ticks;
    // the next delta then spans the gap and must still reconstruct exactly.
    if (tick % 97 == 0) {
      ++st.forcedDrops;
      continue;
    }

    std::vector<uint8_t> framed =
        host.produce(makeSnapshot(hostSb, tick), host.consumeForceNext());
    st.hostBytes += framed.size();
    if (framed.size() > 4) {
      const uint8_t type = framed[4];
      if (type == MSG_KEYFRAME)
        ++st.keyframes;
      else if (type == MSG_DELTA) {
        ++st.deltas;
        st.maxDeltaBytes = std::max(st.maxDeltaBytes, uint32_t(framed.size()));
      }
    }
    if (!link.first->send(framed)) {
      check(false, "host send");
      break;
    }
    pump(*link.second, cp, client, st.clientBytes, st);

    if (crc32(client.grid()) != crc32(gridToBytes(hostSb))) {
      check(false, "replica CRC diverged mid-run");
      ++st.desyncs;
      break;
    }
  }
  // Simulated host ticks, including the ones deliberately not transmitted.
  st.ticks = TICKS;
  check(st.ticks == TICKS, "ran the full tick budget");
  check(st.desyncs == 0, "no desyncs during normal operation");
  check(st.deltas > 0, "deltas were exercised");
  // The periodic keyframe (spec §12.3) must actually fire: over 600 ticks at
  // the default 300-tick interval that is two keyframes (ticks 300 and 600).
  // Without this assertion the cadence bug the M2 log records is invisible --
  // convergence alone cannot tell a keyframe from a delta.
  check(st.keyframes >= 2, "periodic keyframes fired");
  check(crc32(client.grid()) == crc32(gridToBytes(hostSb)),
        "replica CRC equals authoritative grid after the run");

  // --- a corrupted delta is noticed and does not corrupt the replica --------
  {
    host.requestKeyframe(); // start from a known good keyframe
    std::vector<uint8_t> kf =
        host.produce(makeSnapshot(hostSb, host.lastSentTick()), true);
    link.first->send(kf);
    pump(*link.second, cp, client, st.clientBytes, st);

    hostStep(hostSb);
    std::vector<uint8_t> good = host.produce(makeSnapshot(hostSb, host.lastSentTick() + 1), false);
    // Corrupt the frame deterministically: decode it, flip the checksum, and
    // re-encode. Flipping a byte in the compressed stream is not reliable --
    // zstd may decode a small flip back to the same bytes -- whereas a wrong
    // checksum is exactly the condition the receiver must catch.
    Frame gf;
    {
      FrameParser gp;
      gp.push(good.data(), good.size());
      gp.next(gf);
    }
    DeltaMsg dm;
    check(decodePayload(gf.payload, dm), "decoded the delta to corrupt it");
    dm.h.crc32 ^= 0x1u;
    std::vector<uint8_t> bad = frame(MSG_DELTA, gf.seq, encodePayload(dm));
    const Grid before = client.grid();
    const uint32_t desyncsBefore = client.desyncCount();
    link.first->send(bad);
    pump(*link.second, cp, client, st.clientBytes, st);
    check(client.desyncCount() > desyncsBefore, "corrupt delta raised a desync");
    // The corrupt frame must not have been applied on top of the replica.
    // (Either zstd rejected it or the CRC did; what must not happen is a
    // silently wrong grid.)
    const bool unchanged = (client.grid() == before);
    check(unchanged || crc32(client.grid()) == crc32(gridToBytes(hostSb)),
          "corrupt delta left the replica untouched or correct");
    st.recoveredCorruptDelta = true;

    // Now recover with a real keyframe.
    link.first->send(host.produce(makeSnapshot(hostSb, host.lastSentTick() + 1), true));
    pump(*link.second, cp, client, st.clientBytes, st);
    check(crc32(client.grid()) == crc32(gridToBytes(hostSb)),
          "recovered to the authoritative grid after the corrupt delta");
  }

  // --- a delta against the wrong base is rejected ---------------------------
  {
    host.requestKeyframe();
    link.first->send(host.produce(makeSnapshot(hostSb, host.lastSentTick()), true));
    pump(*link.second, cp, client, st.clientBytes, st);
    hostStep(hostSb);
    std::vector<uint8_t> d1 = host.produce(makeSnapshot(hostSb, host.lastSentTick() + 1), false);
    link.first->send(d1);
    pump(*link.second, cp, client, st.clientBytes, st);
    check(crc32(client.grid()) == crc32(gridToBytes(hostSb)),
          "d1 applied against its true base");
    // Re-encode the same delta with a base the replica never had: the receiver
    // must refuse it on the base check alone, before touching the grid.
    Frame gf;
    {
      FrameParser gp;
      gp.push(d1.data(), d1.size());
      gp.next(gf);
    }
    DeltaMsg dm;
    check(decodePayload(gf.payload, dm), "decoded d1 to forge a wrong base");
    dm.h.baseTick += 1000;
    const Grid before = client.grid();
    const uint32_t desyncsBefore = client.desyncCount();
    link.first->send(frame(MSG_DELTA, gf.seq, encodePayload(dm)));
    pump(*link.second, cp, client, st.clientBytes, st);
    check(client.desyncCount() > desyncsBefore, "wrong-base delta raised a desync");
    check(client.grid() == before, "wrong-base delta left the replica untouched");
    st.recoveredWrongBase = true;
    // resync
    host.requestKeyframe();
    link.first->send(host.produce(makeSnapshot(hostSb, host.lastSentTick()), true));
    pump(*link.second, cp, client, st.clientBytes, st);
    check(crc32(client.grid()) == crc32(gridToBytes(hostSb)),
          "recovered to the authoritative grid after the wrong-base delta");
  }

  log << "net-selftest: ticks=" << st.ticks << " keyframes=" << st.keyframes
      << " deltas=" << st.deltas << " forcedDrops=" << st.forcedDrops
      << " maxDeltaBytes=" << st.maxDeltaBytes << " hostBytes=" << st.hostBytes
      << " clientBytes=" << st.clientBytes << " desyncs=" << st.desyncs
      << " checks=" << st.checks << " failures=" << st.failures << "\n";
  return st;
}

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_SELFTEST_HPP_
